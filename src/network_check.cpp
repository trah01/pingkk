#include "pingkk/language.h"
#include "pingkk/network_check.h"
#include "pingkk/core.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <winhttp.h>
#include <netioapi.h>
typedef SOCKET DnsSocket;
typedef int DnsAddressLength;
static const DnsSocket kInvalidDnsSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int DnsSocket;
typedef socklen_t DnsAddressLength;
static const DnsSocket kInvalidDnsSocket = -1;
#endif

#ifdef __APPLE__
#include <SystemConfiguration/SystemConfiguration.h>
#endif

namespace pingkk {
namespace {

class DnsSocketRuntime {
public:
    DnsSocketRuntime() : ready_(true) {
#ifdef _WIN32
        WSADATA data;
        ready_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
    }
    ~DnsSocketRuntime() {
#ifdef _WIN32
        if (ready_) WSACleanup();
#endif
    }
    bool ready() const { return ready_; }
private:
    bool ready_;
};

void closeDnsSocket(DnsSocket socket) {
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

long elapsedSince(const std::chrono::steady_clock::time_point& start) {
    return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count());
}

void appendUnique(std::vector<std::string>& values, const std::string& value) {
    if (!value.empty() &&
        std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

unsigned short readU16(const unsigned char* data) {
    return static_cast<unsigned short>((data[0] << 8) | data[1]);
}

void appendU16(std::vector<unsigned char>& packet, unsigned short value) {
    packet.push_back(static_cast<unsigned char>((value >> 8) & 0xff));
    packet.push_back(static_cast<unsigned char>(value & 0xff));
}

bool appendDnsName(std::vector<unsigned char>& packet, const std::string& host) {
    if (host.empty() || host.size() > 253) return false;
    std::string::size_type start = 0;
    while (start < host.size()) {
        const std::string::size_type dot = host.find('.', start);
        const std::size_t length = (dot == std::string::npos ? host.size() : dot) - start;
        if (length == 0 || length > 63) return false;
        packet.push_back(static_cast<unsigned char>(length));
        packet.insert(packet.end(), host.begin() + start, host.begin() + start + length);
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    packet.push_back(0);
    return true;
}

bool skipDnsName(const unsigned char* packet,
                 std::size_t packetSize,
                 std::size_t& offset) {
    std::size_t labels = 0;
    while (offset < packetSize && labels++ < 128) {
        const unsigned char length = packet[offset++];
        if (length == 0) return true;
        if ((length & 0xc0) == 0xc0) {
            if (offset >= packetSize) return false;
            ++offset;
            return true;
        }
        if ((length & 0xc0) != 0 || offset + length > packetSize) return false;
        offset += length;
    }
    return false;
}

bool makeDnsServerAddress(const std::string& server,
                          sockaddr_storage& address,
                          DnsAddressLength& length) {
    std::memset(&address, 0, sizeof(address));
    sockaddr_in* ipv4 = reinterpret_cast<sockaddr_in*>(&address);
    if (inet_pton(AF_INET, server.c_str(), &ipv4->sin_addr) == 1) {
        ipv4->sin_family = AF_INET;
        ipv4->sin_port = htons(53);
        length = sizeof(sockaddr_in);
        return true;
    }
    sockaddr_in6* ipv6 = reinterpret_cast<sockaddr_in6*>(&address);
    if (server.find('%') == std::string::npos &&
        inet_pton(AF_INET6, server.c_str(), &ipv6->sin6_addr) == 1) {
        ipv6->sin6_family = AF_INET6;
        ipv6->sin6_port = htons(53);
        length = sizeof(sockaddr_in6);
        return true;
    }
    // Link-local DNS servers may include an interface name or scope ID.
    addrinfo hints = {};
    hints.ai_family = AF_INET6;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    addrinfo* scoped = NULL;
    if (getaddrinfo(server.c_str(), "53", &hints, &scoped) == 0 && scoped != NULL) {
        if (scoped->ai_addrlen <= sizeof(address)) {
            std::memcpy(&address, scoped->ai_addr, scoped->ai_addrlen);
            length = static_cast<DnsAddressLength>(scoped->ai_addrlen);
            freeaddrinfo(scoped);
            return true;
        }
        freeaddrinfo(scoped);
    }
    return false;
}

bool queryRecord(const std::string& host,
                 const std::string& server,
                 unsigned short type,
                 int timeoutMilliseconds,
                 std::vector<std::string>& addresses,
                 std::string& error,
                 bool& nameDoesNotExist,
                 bool& noAddressRecords,
                 long& elapsedMilliseconds) {
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    nameDoesNotExist = false;
    noAddressRecords = false;
    DnsSocketRuntime runtime;
    sockaddr_storage destination;
    DnsAddressLength destinationLength = 0;
    if (!runtime.ready() || !makeDnsServerAddress(server, destination, destinationLength)) {
        error = pingkk::text("DNS 服务器地址无效");
        return false;
    }

    const unsigned short identifier = static_cast<unsigned short>(
        std::chrono::steady_clock::now().time_since_epoch().count() & 0xffff);
    std::vector<unsigned char> query;
    appendU16(query, identifier);
    appendU16(query, 0x0100);
    appendU16(query, 1);
    appendU16(query, 0);
    appendU16(query, 0);
    appendU16(query, 0);
    if (!appendDnsName(query, host)) {
        error = pingkk::text("域名格式不正确");
        return false;
    }
    appendU16(query, type);
    appendU16(query, 1);

    const std::size_t initialAddressCount = addresses.size();

    const int family = destination.ss_family;
    DnsSocket socket = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == kInvalidDnsSocket) {
        error = pingkk::text("无法创建 DNS 查询");
        return false;
    }
    if (::connect(socket,
                  reinterpret_cast<sockaddr*>(&destination),
                  destinationLength) != 0) {
        closeDnsSocket(socket);
        error = pingkk::text("无法连接 DNS 服务器");
        return false;
    }
    const int sent = send(socket,
#ifdef _WIN32
                          reinterpret_cast<const char*>(&query[0]),
#else
                          &query[0],
#endif
                          static_cast<int>(query.size()),
                          0);
    if (sent < 0) {
        closeDnsSocket(socket);
        error = pingkk::text("DNS 查询发送失败");
        return false;
    }

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(socket, &readSet);
    timeval timeout;
    timeout.tv_sec = timeoutMilliseconds / 1000;
    timeout.tv_usec = (timeoutMilliseconds % 1000) * 1000;
    const int selected = select(static_cast<int>(socket) + 1,
                                &readSet, NULL, NULL, &timeout);
    if (selected <= 0) {
        closeDnsSocket(socket);
        elapsedMilliseconds = elapsedSince(start);
        error = selected == 0 ? pingkk::text("查询超时") : pingkk::text("DNS 查询失败");
        return false;
    }

    unsigned char response[4096];
    const int received = recv(socket,
#ifdef _WIN32
                              reinterpret_cast<char*>(response),
#else
                              response,
#endif
                              sizeof(response), 0);
    closeDnsSocket(socket);
    elapsedMilliseconds = elapsedSince(start);
    if (received < 12 || readU16(response) != identifier) {
        error = pingkk::text("DNS 响应无效");
        return false;
    }
    const unsigned short flags = readU16(response + 2);
    if ((flags & 0x8000) == 0) {
        error = pingkk::text("收到的 DNS 数据不是响应");
        return false;
    }
    if ((flags & 0x0200) != 0) {
        error = pingkk::text("DNS 响应被截断");
        return false;
    }
    const int responseCode = flags & 0x0f;
    if (responseCode != 0) {
        nameDoesNotExist = responseCode == 3;
        if (nameDoesNotExist) {
            error = pingkk::text("域名不存在（NXDOMAIN）");
        } else {
            std::ostringstream message;
            message << pingkk::text("DNS 返回错误码 ") << responseCode;
            error = message.str();
        }
        return false;
    }

    const std::size_t responseSize = static_cast<std::size_t>(received);
    const unsigned short questions = readU16(response + 4);
    const unsigned short answers = readU16(response + 6);
    if (questions != 1) {
        error = pingkk::text("DNS 响应中的查询数量不正确");
        return false;
    }
    std::size_t offset = 12;
    for (unsigned short index = 0; index < questions; ++index) {
        if (!skipDnsName(response, responseSize, offset) || offset + 4 > responseSize) {
            error = pingkk::text("DNS 响应格式错误");
            return false;
        }
        if (readU16(response + offset) != type ||
            readU16(response + offset + 2) != 1) {
            error = pingkk::text("DNS 响应与查询不匹配");
            return false;
        }
        offset += 4;
    }
    for (unsigned short index = 0; index < answers; ++index) {
        if (!skipDnsName(response, responseSize, offset) || offset + 10 > responseSize) {
            error = pingkk::text("DNS 响应格式错误");
            return false;
        }
        const unsigned short recordType = readU16(response + offset);
        const unsigned short recordClass = readU16(response + offset + 2);
        const unsigned short dataLength = readU16(response + offset + 8);
        offset += 10;
        if (offset + dataLength > responseSize) {
            error = pingkk::text("DNS 响应格式错误");
            return false;
        }
        char text[INET6_ADDRSTRLEN] = {0};
        if (recordClass == 1 && recordType == 1 && dataLength == 4 &&
            inet_ntop(AF_INET, response + offset, text, sizeof(text)) != NULL) {
            appendUnique(addresses, text);
        } else if (recordClass == 1 && recordType == 28 && dataLength == 16 &&
                   inet_ntop(AF_INET6, response + offset, text, sizeof(text)) != NULL) {
            appendUnique(addresses, text);
        }
        offset += dataLength;
    }
    if (addresses.size() == initialAddressCount) {
        noAddressRecords = true;
        error = type == 1 ? pingkk::text("没有返回 A 记录") : pingkk::text("没有返回 AAAA 记录");
        return false;
    }
    return true;
}

DnsLookupResult queryResolver(const std::string& host,
                              const std::string& name,
                              const std::string& server,
                              int timeoutMilliseconds,
                              IpVersion version) {
    DnsLookupResult result;
    result.resolverName = name;
    result.resolverAddress = server;
    result.success = false;
    result.nameDoesNotExist = false;
    result.noAddressRecords = false;
    result.elapsedMilliseconds = 0;
    long ipv4Elapsed = 0;
    long ipv6Elapsed = 0;
    std::string ipv4Error;
    std::string ipv6Error;
    bool ipv4NameDoesNotExist = false;
    bool ipv6NameDoesNotExist = false;
    bool ipv4NoAddressRecords = false;
    bool ipv6NoAddressRecords = false;
    const bool ipv4Ok = version != IpVersion::V6 && queryRecord(host, server, 1, timeoutMilliseconds,
                                    result.ipv4Addresses, ipv4Error,
                                    ipv4NameDoesNotExist, ipv4NoAddressRecords,
                                    ipv4Elapsed);
    const bool ipv6Ok = version != IpVersion::V4 && queryRecord(host, server, 28, timeoutMilliseconds,
                                    result.ipv6Addresses, ipv6Error,
                                    ipv6NameDoesNotExist, ipv6NoAddressRecords,
                                    ipv6Elapsed);
    result.elapsedMilliseconds = ipv4Elapsed + ipv6Elapsed;
    result.success = ipv4Ok || ipv6Ok;
    result.nameDoesNotExist = version == IpVersion::V4 ? ipv4NameDoesNotExist :
                             version == IpVersion::V6 ? ipv6NameDoesNotExist :
                             ipv4NameDoesNotExist && ipv6NameDoesNotExist;
    result.noAddressRecords = version == IpVersion::V4 ? ipv4NoAddressRecords :
                             version == IpVersion::V6 ? ipv6NoAddressRecords :
                             ipv4NoAddressRecords && ipv6NoAddressRecords;
    if (!result.success) {
        result.error = ipv4Error.empty() ? ipv6Error :
                       ipv4Error == ipv6Error || ipv6Error.empty()
                           ? ipv4Error
                           : ipv4Error + pingkk::text("；") + ipv6Error;
    }
    return result;
}

DnsLookupResult querySystemResolver(const std::string& host, IpVersion version) {
    DnsLookupResult result;
    result.resolverName = pingkk::text("系统 DNS");
    result.resolverAddress = pingkk::text("自动");
    result.success = false;
    result.nameDoesNotExist = false;
    result.noAddressRecords = false;
    result.elapsedMilliseconds = 0;
    DnsSocketRuntime runtime;
    if (!runtime.ready()) {
        result.error = pingkk::text("网络组件初始化失败");
        return result;
    }
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = version == IpVersion::V6 ? AF_INET6 :
                      version == IpVersion::V4 ? AF_INET : AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* records = NULL;
    const int status = getaddrinfo(host.c_str(), NULL, &hints, &records);
    result.elapsedMilliseconds = elapsedSince(start);
    if (status != 0 || records == NULL) {
        result.error = pingkk::text("系统解析失败");
        return result;
    }
    for (addrinfo* current = records; current != NULL; current = current->ai_next) {
        if (current->ai_family == AF_INET6 &&
            IN6_IS_ADDR_V4MAPPED(&reinterpret_cast<const sockaddr_in6*>(current->ai_addr)->sin6_addr)) continue;
        char text[NI_MAXHOST] = {0};
        if (getnameinfo(current->ai_addr,
                        static_cast<DnsAddressLength>(current->ai_addrlen),
                        text, sizeof(text), NULL, 0, NI_NUMERICHOST) != 0) continue;
        if (current->ai_family == AF_INET) appendUnique(result.ipv4Addresses, text);
        if (current->ai_family == AF_INET6) appendUnique(result.ipv6Addresses, text);
    }
    freeaddrinfo(records);
    result.success = !result.ipv4Addresses.empty() || !result.ipv6Addresses.empty();
    if (!result.success) result.error = pingkk::text("没有返回 A 或 AAAA 记录");
    return result;
}

std::string safeProxyValue(const char* raw) {
    if (raw == NULL || *raw == '\0') return std::string();
    std::string value(raw);
    const std::string::size_type scheme = value.find("://");
    const std::string prefix = scheme == std::string::npos
                                   ? std::string()
                                   : value.substr(0, scheme + 3);
    std::string remainder = scheme == std::string::npos ? value : value.substr(scheme + 3);
    const std::string::size_type at = remainder.rfind('@');
    if (at != std::string::npos) remainder = remainder.substr(at + 1);
    const std::string::size_type path = remainder.find('/');
    if (path != std::string::npos) remainder = remainder.substr(0, path);
    return prefix + remainder;
}

std::string safeProxyList(const std::string& raw) {
    std::ostringstream safe;
    std::string::size_type start = 0;
    while (start <= raw.size()) {
        const std::string::size_type separator = raw.find(';', start);
        const std::string item = raw.substr(
            start, separator == std::string::npos ? std::string::npos : separator - start);
        const std::string::size_type equals = item.find('=');
        if (safe.tellp() > 0) safe << ';';
        if (equals == std::string::npos) {
            safe << safeProxyValue(item.c_str());
        } else {
            safe << item.substr(0, equals + 1)
                 << safeProxyValue(item.substr(equals + 1).c_str());
        }
        if (separator == std::string::npos) break;
        start = separator + 1;
    }
    return safe.str();
}

#ifdef _WIN32
std::string utf8FromWide(const wchar_t* value) {
    if (value == NULL || *value == L'\0') return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, NULL, 0, NULL, NULL);
    if (size <= 1) return std::string();
    std::vector<char> buffer(static_cast<std::size_t>(size), 0);
    WideCharToMultiByte(CP_UTF8, 0, value, -1, &buffer[0], size, NULL, NULL);
    return std::string(&buffer[0]);
}
#endif

std::string ipv6Gateway() {
#ifdef _WIN32
    DnsSocketRuntime runtime;
    if (!runtime.ready()) return pingkk::text("未知");
    SOCKADDR_INET destination = {};
    destination.Ipv6.sin6_family = AF_INET6;
    inet_pton(AF_INET6, "2400:3200::1", &destination.Ipv6.sin6_addr);
    MIB_IPFORWARD_ROW2 route = {};
    SOCKADDR_INET source = {};
    if (GetBestRoute2(NULL, 0, NULL, &destination, 0, &route, &source) == NO_ERROR) {
        char text[INET6_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET6, &route.NextHop.Ipv6.sin6_addr, text, sizeof(text)) != NULL) {
            std::string gateway(text);
            if (IN6_IS_ADDR_LINKLOCAL(&route.NextHop.Ipv6.sin6_addr)) {
                gateway += "%" + std::to_string(route.InterfaceIndex);
            }
            return gateway;
        }
    }
#elif defined(__APPLE__)
    SCDynamicStoreRef store = SCDynamicStoreCreate(NULL, CFSTR("pingkk-gateway6"), NULL, NULL);
    if (store != NULL) {
        CFDictionaryRef value = static_cast<CFDictionaryRef>(
            SCDynamicStoreCopyValue(store, CFSTR("State:/Network/Global/IPv6")));
        if (value != NULL) {
            CFStringRef router = static_cast<CFStringRef>(CFDictionaryGetValue(value, CFSTR("Router")));
            char text[256] = {0};
            if (router != NULL && CFStringGetCString(router, text, sizeof(text), kCFStringEncodingUTF8)) {
                CFRelease(value);
                CFRelease(store);
                return text;
            }
            CFRelease(value);
        }
        CFRelease(store);
    }
#else
    std::ifstream routes("/proc/net/ipv6_route");
    std::string line;
    std::string bestGateway;
    unsigned long bestMetric = ~0ul;
    while (std::getline(routes, line)) {
        std::istringstream stream(line);
        std::string destination, prefix, source, sourcePrefix, gateway;
        std::string metric, references, uses, flags, interfaceName;
        if (!(stream >> destination >> prefix >> source >> sourcePrefix >> gateway
                     >> metric >> references >> uses >> flags >> interfaceName)) continue;
        const unsigned long routeFlags = std::strtoul(flags.c_str(), NULL, 16);
        const unsigned long routeMetric = std::strtoul(metric.c_str(), NULL, 16);
        if (destination != std::string(32, '0') || prefix != "00" || sourcePrefix != "00" ||
            gateway.size() != 32 || !(routeFlags & 1) || (routeFlags & 0x200) ||
            routeMetric >= bestMetric) continue;
        in6_addr address = {};
        for (int index = 0; index < 16; ++index) {
            address.s6_addr[index] = static_cast<unsigned char>(
                std::strtoul(gateway.substr(index * 2, 2).c_str(), NULL, 16));
        }
        char text[INET6_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET6, &address, text, sizeof(text)) == NULL) continue;
        bestGateway = text;
        if (IN6_IS_ADDR_LINKLOCAL(&address)) bestGateway += "%" + interfaceName;
        bestMetric = routeMetric;
    }
    if (!bestGateway.empty()) return bestGateway;
#endif
    return pingkk::text("未知");
}

}  // namespace

std::vector<DnsLookupResult> compareDns(const std::string& input,
                                        int timeoutMilliseconds,
                                        IpVersion version) {
    const std::string host = extractHost(input);
    std::vector<DnsLookupResult> results;
    if (host.empty()) return results;
    results.push_back(querySystemResolver(host, version));

    std::vector<std::pair<std::string, std::string> > resolvers;
    const std::vector<std::string> systemServers = currentDnsServers();
    for (std::size_t index = 0; index < systemServers.size(); ++index) {
        const bool ipv6 = systemServers[index].find(':') != std::string::npos;
        if ((version == IpVersion::V6 && !ipv6) || (version == IpVersion::V4 && ipv6)) continue;
        resolvers.push_back(std::make_pair(pingkk::text("当前 DNS"), systemServers[index]));
    }
    resolvers.push_back(std::make_pair(pingkk::text("阿里 DNS"), version == IpVersion::V6 ? "2400:3200::1" : "223.5.5.5"));
    resolvers.push_back(std::make_pair(pingkk::text("腾讯 DNS"), version == IpVersion::V6 ? "2402:4e00::" : "119.29.29.29"));

    std::set<std::string> queried;
    for (std::size_t index = 0; index < resolvers.size(); ++index) {
        if (!queried.insert(resolvers[index].second).second) continue;
        results.push_back(queryResolver(host,
                                        resolvers[index].first,
                                        resolvers[index].second,
                                        timeoutMilliseconds, version));
    }
    return results;
}

std::string defaultGateway(IpVersion version) {
    if (version == IpVersion::V6) return ipv6Gateway();
#ifdef _WIN32
    MIB_IPFORWARDROW route;
    if (GetBestRoute(inet_addr("223.5.5.5"), 0, &route) == NO_ERROR) {
        in_addr address;
        address.s_addr = route.dwForwardNextHop;
        char text[INET_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET, &address, text, sizeof(text)) != NULL) return text;
    }
#elif defined(__APPLE__)
    SCDynamicStoreRef store = SCDynamicStoreCreate(NULL, CFSTR("pingkk-gateway"), NULL, NULL);
    if (store != NULL) {
        CFDictionaryRef value = static_cast<CFDictionaryRef>(
            SCDynamicStoreCopyValue(store, CFSTR("State:/Network/Global/IPv4")));
        if (value != NULL) {
            CFStringRef router = static_cast<CFStringRef>(
                CFDictionaryGetValue(value, CFSTR("Router")));
            char text[256] = {0};
            if (router != NULL && CFStringGetCString(
                    router, text, sizeof(text), kCFStringEncodingUTF8)) {
                CFRelease(value);
                CFRelease(store);
                return text;
            }
            CFRelease(value);
        }
        CFRelease(store);
    }
#else
    std::ifstream routes("/proc/net/route");
    std::string line;
    std::getline(routes, line);
    while (std::getline(routes, line)) {
        std::istringstream stream(line);
        std::string interfaceName;
        std::string destination;
        std::string gateway;
        stream >> interfaceName >> destination >> gateway;
        if (destination != "00000000" || gateway.size() != 8) continue;
        unsigned long raw = std::strtoul(gateway.c_str(), NULL, 16);
        in_addr address;
        address.s_addr = static_cast<unsigned int>(raw);
        char text[INET_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET, &address, text, sizeof(text)) != NULL) return text;
    }
#endif
    return pingkk::text("未知");
}

std::vector<std::string> currentProxySettings() {
    std::vector<std::string> proxies;
    const char* names[] = {"HTTPS_PROXY", "HTTP_PROXY", "ALL_PROXY",
                           "https_proxy", "http_proxy", "all_proxy"};
    for (std::size_t index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
        const std::string value = safeProxyValue(std::getenv(names[index]));
        if (!value.empty()) appendUnique(proxies, std::string(names[index]) + "=" + value);
    }
#ifdef __APPLE__
    CFDictionaryRef settings = SCDynamicStoreCopyProxies(NULL);
    if (settings != NULL) {
        struct ProxyKey { CFStringRef enabled; CFStringRef host; CFStringRef port; const char* name; };
        const ProxyKey keys[] = {
            {kSCPropNetProxiesHTTPEnable, kSCPropNetProxiesHTTPProxy, kSCPropNetProxiesHTTPPort, pingkk::text("系统 HTTP")},
            {kSCPropNetProxiesHTTPSEnable, kSCPropNetProxiesHTTPSProxy, kSCPropNetProxiesHTTPSPort, pingkk::text("系统 HTTPS")},
            {kSCPropNetProxiesSOCKSEnable, kSCPropNetProxiesSOCKSProxy, kSCPropNetProxiesSOCKSPort, pingkk::text("系统 SOCKS")}
        };
        for (std::size_t index = 0; index < sizeof(keys) / sizeof(keys[0]); ++index) {
            CFNumberRef enabled = static_cast<CFNumberRef>(CFDictionaryGetValue(settings, keys[index].enabled));
            int enabledValue = 0;
            if (enabled == NULL || !CFNumberGetValue(enabled, kCFNumberIntType, &enabledValue) || !enabledValue) continue;
            CFStringRef host = static_cast<CFStringRef>(CFDictionaryGetValue(settings, keys[index].host));
            CFNumberRef port = static_cast<CFNumberRef>(CFDictionaryGetValue(settings, keys[index].port));
            char hostText[256] = {0};
            int portValue = 0;
            if (host != NULL) CFStringGetCString(host, hostText, sizeof(hostText), kCFStringEncodingUTF8);
            if (port != NULL) CFNumberGetValue(port, kCFNumberIntType, &portValue);
            std::ostringstream description;
            description << keys[index].name << '=' << hostText;
            if (portValue > 0) description << ':' << portValue;
            appendUnique(proxies, description.str());
        }
        CFRelease(settings);
    }
#elif defined(_WIN32)
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG configuration;
    std::memset(&configuration, 0, sizeof(configuration));
    if (WinHttpGetIEProxyConfigForCurrentUser(&configuration)) {
        const std::string proxy = utf8FromWide(configuration.lpszProxy);
        if (!proxy.empty()) appendUnique(proxies, pingkk::text("系统代理=") + safeProxyList(proxy));
        if (configuration.fAutoDetect) appendUnique(proxies, pingkk::text("系统自动代理检测=已启用"));
        if (configuration.lpszAutoConfigUrl != NULL) {
            appendUnique(proxies, pingkk::text("系统 PAC=已启用"));
        }
        if (configuration.lpszAutoConfigUrl != NULL) GlobalFree(configuration.lpszAutoConfigUrl);
        if (configuration.lpszProxy != NULL) GlobalFree(configuration.lpszProxy);
        if (configuration.lpszProxyBypass != NULL) GlobalFree(configuration.lpszProxyBypass);
    }
#endif
    return proxies;
}

}  // namespace pingkk
