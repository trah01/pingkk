#include "pingkk/language.h"
#include "pingkk/core.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
typedef SOCKET SocketHandle;
typedef int AddressLength;
static const SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SocketHandle;
typedef socklen_t AddressLength;
static const SocketHandle kInvalidSocket = -1;
#endif

#ifdef __APPLE__
#include <SystemConfiguration/SystemConfiguration.h>
#endif

namespace pingkk {
namespace {

class SocketRuntime {
public:
    SocketRuntime() : ready_(true) {
#ifdef _WIN32
        WSADATA data;
        ready_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
    }

    ~SocketRuntime() {
#ifdef _WIN32
        if (ready_) {
            WSACleanup();
        }
#endif
    }

    bool ready() const { return ready_; }

private:
    bool ready_;
};

void closeSocket(SocketHandle socket) {
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

bool setNonBlocking(SocketHandle socket, bool enabled) {
#ifdef _WIN32
    u_long mode = enabled ? 1 : 0;
    return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 &&
           fcntl(socket, F_SETFL,
                 enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
#endif
}

int socketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool connectInProgress(int error) {
#ifdef _WIN32
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
    return error == EINPROGRESS || error == EWOULDBLOCK;
#endif
}

std::string dnsErrorText(int error) {
    if (error == EAI_AGAIN) {
        return pingkk::text("域名解析暂时失败，请稍后重试");
    }
    if (error == EAI_NONAME) {
        return pingkk::text("未找到该域名，请检查地址是否正确");
    }
#ifdef EAI_NODATA
    if (error == EAI_NODATA) {
        return pingkk::text("该域名没有可用的 IP 地址");
    }
#endif
    if (error == EAI_FAIL) {
        return pingkk::text("域名解析服务器返回错误");
    }
#ifdef EAI_SYSTEM
    if (error == EAI_SYSTEM) {
        return pingkk::text("域名解析失败，请检查网络或 DNS 设置");
    }
#endif
    return pingkk::text("域名解析失败，请检查地址和网络设置");
}

std::string errorText(int error) {
#ifdef _WIN32
    std::ostringstream stream;
    stream << pingkk::text("系统错误 ") << error;
    return stream.str();
#else
    return std::strerror(error);
#endif
}

bool makeAddress(const ResolvedTarget& target,
                 unsigned short port,
                 sockaddr_storage& storage,
                 AddressLength& length) {
    std::memset(&storage, 0, sizeof(storage));
    std::ostringstream service;
    service << port;
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    addrinfo* result = NULL;
    if (getaddrinfo(target.ip.c_str(), service.str().c_str(), &hints, &result) != 0 ||
        result == NULL || result->ai_addrlen > sizeof(storage)) {
        if (result != NULL) freeaddrinfo(result);
        return false;
    }
    std::memcpy(&storage, result->ai_addr, result->ai_addrlen);
    length = static_cast<AddressLength>(result->ai_addrlen);
    freeaddrinfo(result);
    return true;
}

long elapsedMilliseconds(const std::chrono::steady_clock::time_point& start) {
    return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count());
}

void appendUnique(std::vector<std::string>& values, const std::string& value) {
    if (!value.empty() &&
        std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

void appendResolvConfServers(std::vector<std::string>& servers) {
    std::ifstream configuration("/etc/resolv.conf");
    std::string line;
    while (std::getline(configuration, line)) {
        std::istringstream stream(line);
        std::string directive;
        std::string address;
        stream >> directive >> address;
        if (directive == "nameserver") appendUnique(servers, address);
    }
}

#ifdef __APPLE__
void appendDnsDictionary(CFDictionaryRef dictionary,
                         std::vector<std::string>& servers) {
    if (dictionary == NULL) return;
    CFArrayRef addresses = static_cast<CFArrayRef>(
        CFDictionaryGetValue(dictionary, kSCPropNetDNSServerAddresses));
    if (addresses == NULL || CFGetTypeID(addresses) != CFArrayGetTypeID()) return;

    const CFIndex count = CFArrayGetCount(addresses);
    for (CFIndex index = 0; index < count; ++index) {
        CFStringRef address = static_cast<CFStringRef>(
            CFArrayGetValueAtIndex(addresses, index));
        if (address == NULL || CFGetTypeID(address) != CFStringGetTypeID()) continue;
        char buffer[256] = {0};
        if (CFStringGetCString(address, buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
            appendUnique(servers, buffer);
        }
    }
}
#endif

}  // namespace

std::string extractHost(const std::string& input) {
    std::string value = input;
    const std::string::size_type first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return std::string();
    }
    value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);

    const std::string::size_type scheme = value.find("://");
    if (scheme != std::string::npos) {
        value = value.substr(scheme + 3);
    }
    const std::string::size_type at = value.find('@');
    if (at != std::string::npos) {
        value = value.substr(at + 1);
    }
    value = value.substr(0, value.find_first_of("/?#"));

    if (!value.empty() && value[0] == '[') {
        const std::string::size_type closing = value.find(']');
        return closing == std::string::npos ? std::string() : value.substr(1, closing - 1);
    }

    const std::string::size_type colon = value.rfind(':');
    if (colon != std::string::npos && value.find(':') == colon) {
        value = value.substr(0, colon);
    }
    return value;
}

bool resolveTarget(const std::string& input,
                   ResolvedTarget& target,
                   std::string& error,
                   IpVersion version) {
    target.input = input;
    target.host = extractHost(input);
    target.ip.clear();
    if (target.host.empty()) {
        error = pingkk::text("目标地址为空或格式不正确");
        return false;
    }

    SocketRuntime runtime;
    if (!runtime.ready()) {
        error = pingkk::text("网络组件初始化失败");
        return false;
    }

    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = version == IpVersion::V6 ? AF_INET6 :
                      version == IpVersion::V4 ? AF_INET : AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = NULL;
    const int status = getaddrinfo(target.host.c_str(), NULL, &hints, &results);
    if (status != 0 || results == NULL) {
        error = dnsErrorText(status);
        return false;
    }

    const addrinfo* selected = NULL;
    for (const addrinfo* current = results; current != NULL; current = current->ai_next) {
        if (current->ai_family == AF_INET6 &&
            IN6_IS_ADDR_V4MAPPED(&reinterpret_cast<const sockaddr_in6*>(current->ai_addr)->sin6_addr)) {
            continue;
        }
        if ((version == IpVersion::V4 && current->ai_family != AF_INET) ||
            (version == IpVersion::V6 && current->ai_family != AF_INET6)) continue;
        if (selected == NULL) selected = current;
        if (current->ai_family == AF_INET) {
            selected = current;
            break;
        }
    }
    if (selected == NULL) {
        freeaddrinfo(results);
        error = version == IpVersion::V6 ? text("目标没有可用的 IPv6 地址")
                                        : text("目标没有可用的 IPv4 地址");
        return false;
    }

    char buffer[NI_MAXHOST] = {0};
    if (getnameinfo(selected->ai_addr,
                    static_cast<AddressLength>(selected->ai_addrlen),
                    buffer,
                    sizeof(buffer),
                    NULL,
                    0,
                    NI_NUMERICHOST) == 0) {
        target.ip = buffer;
    }
    freeaddrinfo(results);

    if (target.ip.empty()) {
        error = pingkk::text("解析结果中没有可用的 IP 地址");
        return false;
    }
    return true;
}

std::string localAddressFor(const ResolvedTarget& target) {
    SocketRuntime runtime;
    sockaddr_storage remote;
    AddressLength remoteLength = 0;
    if (!runtime.ready() || !makeAddress(target, 53, remote, remoteLength)) {
        return pingkk::text("未知");
    }

    const int family = target.ip.find(':') == std::string::npos ? AF_INET : AF_INET6;
    SocketHandle socket = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == kInvalidSocket) {
        return pingkk::text("未知");
    }
    if (::connect(socket, reinterpret_cast<sockaddr*>(&remote), remoteLength) != 0) {
        closeSocket(socket);
        return pingkk::text("未知");
    }

    sockaddr_storage local;
    AddressLength localLength = sizeof(local);
    std::memset(&local, 0, sizeof(local));
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&local), &localLength) != 0) {
        closeSocket(socket);
        return pingkk::text("未知");
    }
    closeSocket(socket);

    char buffer[NI_MAXHOST] = {0};
    return getnameinfo(reinterpret_cast<sockaddr*>(&local),
                       localLength,
                       buffer,
                       sizeof(buffer),
                       NULL,
                       0,
                       NI_NUMERICHOST) == 0
               ? buffer
               : pingkk::text("未知");
}

std::string primaryLocalAddress(IpVersion version) {
    ResolvedTarget routeTarget;
    routeTarget.ip = version == IpVersion::V6 ? "2400:3200::1" : "223.5.5.5";
    routeTarget.input = routeTarget.ip;
    routeTarget.host = routeTarget.ip;
    return localAddressFor(routeTarget);
}

std::string subnetMaskFor(const std::string& localAddress) {
    if (localAddress.find(':') != std::string::npos) return pingkk::text("未知");
    return networkPrefixFor(localAddress);
}

std::string networkPrefixFor(const std::string& localAddress) {
    const bool ipv6 = localAddress.find(':') != std::string::npos;
    const int family = ipv6 ? AF_INET6 : AF_INET;
    unsigned char local[16] = {0};
    const std::string numeric = localAddress.substr(0, localAddress.find('%'));
    if (inet_pton(family, numeric.c_str(), local) != 1) return pingkk::text("未知");
#ifdef _WIN32
    SocketRuntime runtime;
    if (!runtime.ready()) return pingkk::text("未知");
    ULONG size = 16384;
    std::vector<unsigned char> buffer(size);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                        GAA_FLAG_SKIP_DNS_SERVER;
    ULONG status = GetAdaptersAddresses(family, flags, NULL,
        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(&buffer[0]), &size);
    if (status == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        status = GetAdaptersAddresses(family, flags, NULL,
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(&buffer[0]), &size);
    }
    if (status != NO_ERROR) return pingkk::text("未知");
    for (IP_ADAPTER_ADDRESSES* adapter =
             reinterpret_cast<IP_ADAPTER_ADDRESSES*>(&buffer[0]);
         adapter != NULL; adapter = adapter->Next) {
        for (IP_ADAPTER_UNICAST_ADDRESS* entry = adapter->FirstUnicastAddress;
             entry != NULL; entry = entry->Next) {
            const sockaddr* address = entry->Address.lpSockaddr;
            if (address == NULL || address->sa_family != family) continue;
            const void* bytes = ipv6
                ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in6*>(address)->sin6_addr)
                : static_cast<const void*>(&reinterpret_cast<const sockaddr_in*>(address)->sin_addr);
            if (std::memcmp(bytes, local, ipv6 ? 16 : 4) != 0) continue;
            const unsigned int prefix = entry->OnLinkPrefixLength;
            if (prefix > (ipv6 ? 128u : 32u)) continue;
            if (ipv6) return "/" + std::to_string(prefix);
            in_addr mask;
            mask.s_addr = htonl(prefix == 0 ? 0 : 0xffffffffu << (32 - prefix));
            char text[INET_ADDRSTRLEN] = {0};
            if (inet_ntop(AF_INET, &mask, text, sizeof(text)) != NULL) return text;
        }
    }
#else
    ifaddrs* interfaces = NULL;
    if (getifaddrs(&interfaces) != 0) return pingkk::text("未知");
    std::string mask = pingkk::text("未知");
    for (const ifaddrs* entry = interfaces; entry != NULL; entry = entry->ifa_next) {
        if (entry->ifa_addr == NULL || entry->ifa_netmask == NULL ||
            entry->ifa_addr->sa_family != family) continue;
        const void* bytes = ipv6
            ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in6*>(entry->ifa_addr)->sin6_addr)
            : static_cast<const void*>(&reinterpret_cast<const sockaddr_in*>(entry->ifa_addr)->sin_addr);
        if (std::memcmp(bytes, local, ipv6 ? 16 : 4) != 0) continue;
        if (ipv6) {
            const unsigned char* netmask = reinterpret_cast<const sockaddr_in6*>(entry->ifa_netmask)->sin6_addr.s6_addr;
            unsigned int prefix = 0;
            for (int index = 0; index < 16; ++index) {
                for (unsigned char bit = 0x80; bit != 0; bit >>= 1) {
                    if (netmask[index] & bit) ++prefix;
                }
            }
            mask = "/" + std::to_string(prefix);
        } else {
            const sockaddr_in* netmask = reinterpret_cast<const sockaddr_in*>(entry->ifa_netmask);
            char text[INET_ADDRSTRLEN] = {0};
            if (inet_ntop(AF_INET, &netmask->sin_addr, text, sizeof(text)) != NULL) mask = text;
        }
        break;
    }
    freeifaddrs(interfaces);
    return mask;
#endif
    return pingkk::text("未知");
}

std::vector<std::string> currentDnsServers() {
    std::vector<std::string> servers;
#ifdef _WIN32
    SocketRuntime runtime;
    if (!runtime.ready()) return servers;
    ULONG size = 16384;
    std::vector<unsigned char> buffer(size);
    const ULONG flags = GAA_FLAG_SKIP_UNICAST | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;
    ULONG status = GetAdaptersAddresses(AF_UNSPEC, flags, NULL,
        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(&buffer[0]), &size);
    if (status == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        status = GetAdaptersAddresses(AF_UNSPEC, flags, NULL,
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(&buffer[0]), &size);
    }
    if (status != NO_ERROR) return servers;
    for (IP_ADAPTER_ADDRESSES* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(&buffer[0]);
         adapter != NULL; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) continue;
        for (IP_ADAPTER_DNS_SERVER_ADDRESS* entry = adapter->FirstDnsServerAddress;
             entry != NULL; entry = entry->Next) {
            if (entry->Address.lpSockaddr == NULL) continue;
            char text[NI_MAXHOST] = {0};
            if (getnameinfo(entry->Address.lpSockaddr, entry->Address.iSockaddrLength,
                            text, sizeof(text), NULL, 0, NI_NUMERICHOST) == 0) {
                appendUnique(servers, text);
            }
        }
    }
#elif defined(__APPLE__)
    SCDynamicStoreRef store = SCDynamicStoreCreate(
        NULL, CFSTR("pingkk"), NULL, NULL);
    if (store == NULL) {
        appendResolvConfServers(servers);
        return servers;
    }
    CFArrayRef keys = SCDynamicStoreCopyKeyList(
        store, CFSTR("State:/Network/(Global|Service/.+)/DNS"));
    if (keys != NULL) {
        const CFIndex count = CFArrayGetCount(keys);
        for (CFIndex index = 0; index < count; ++index) {
            CFStringRef key = static_cast<CFStringRef>(
                CFArrayGetValueAtIndex(keys, index));
            CFPropertyListRef value = SCDynamicStoreCopyValue(store, key);
            if (value != NULL && CFGetTypeID(value) == CFDictionaryGetTypeID()) {
                appendDnsDictionary(static_cast<CFDictionaryRef>(value), servers);
            }
            if (value != NULL) CFRelease(value);
        }
        CFRelease(keys);
    }
    CFRelease(store);
    if (servers.empty()) appendResolvConfServers(servers);
#else
    appendResolvConfServers(servers);
#endif
    return servers;
}

ProbeResult probeTcp(const ResolvedTarget& target,
                     unsigned short port,
                     int timeoutMilliseconds) {
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    SocketRuntime runtime;
    ProbeResult result = {Protocol::Tcp, false, true, 0, std::string()};
    sockaddr_storage address;
    AddressLength addressLength = 0;
    if (!runtime.ready() || !makeAddress(target, port, address, addressLength)) {
        result.message = pingkk::text("无法创建目标地址");
        return result;
    }

    const int family = target.ip.find(':') == std::string::npos ? AF_INET : AF_INET6;
    SocketHandle socket = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (socket == kInvalidSocket || !setNonBlocking(socket, true)) {
        result.message = pingkk::text("无法创建 TCP 连接");
        if (socket != kInvalidSocket) closeSocket(socket);
        return result;
    }

    int status = ::connect(socket, reinterpret_cast<sockaddr*>(&address), addressLength);
    if (status != 0 && !connectInProgress(socketError())) {
        result.message = errorText(socketError());
        closeSocket(socket);
        result.elapsedMilliseconds = elapsedMilliseconds(start);
        return result;
    }

    if (status != 0) {
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(socket, &writeSet);
        timeval timeout;
        timeout.tv_sec = timeoutMilliseconds / 1000;
        timeout.tv_usec = (timeoutMilliseconds % 1000) * 1000;
        status = select(static_cast<int>(socket) + 1, NULL, &writeSet, NULL, &timeout);
        if (status > 0) {
            int pendingError = 0;
            AddressLength length = sizeof(pendingError);
            const int optionStatus = getsockopt(socket, SOL_SOCKET, SO_ERROR,
#ifdef _WIN32
                       reinterpret_cast<char*>(&pendingError),
#else
                       &pendingError,
#endif
                       &length);
            if (optionStatus != 0) {
                result.message = errorText(socketError());
            } else {
                result.reachable = pendingError == 0;
                result.message = result.reachable ? pingkk::text("连接成功") : errorText(pendingError);
            }
        } else {
            result.message = status == 0 ? pingkk::text("连接超时") : errorText(socketError());
        }
    } else {
        result.reachable = true;
        result.message = pingkk::text("连接成功");
    }

    closeSocket(socket);
    result.elapsedMilliseconds = elapsedMilliseconds(start);
    return result;
}

ProbeResult probeUdp(const ResolvedTarget& target,
                     unsigned short port,
                     int timeoutMilliseconds) {
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    SocketRuntime runtime;
    ProbeResult result = {Protocol::Udp, false, false, 0, std::string()};
    sockaddr_storage address;
    AddressLength addressLength = 0;
    if (!runtime.ready() || !makeAddress(target, port, address, addressLength)) {
        result.message = pingkk::text("无法创建目标地址");
        return result;
    }

    const int family = target.ip.find(':') == std::string::npos ? AF_INET : AF_INET6;
    SocketHandle socket = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == kInvalidSocket) {
        result.message = pingkk::text("无法创建 UDP 探测");
        return result;
    }

    if (::connect(socket, reinterpret_cast<sockaddr*>(&address), addressLength) != 0) {
        result.message = errorText(socketError());
        closeSocket(socket);
        return result;
    }

    const char payload[] = "pingkk";
    const int sent = send(socket, payload, static_cast<int>(sizeof(payload) - 1), 0);
    if (sent < 0) {
        result.message = errorText(socketError());
        result.definitive = true;
        closeSocket(socket);
        return result;
    }

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(socket, &readSet);
    timeval timeout;
    timeout.tv_sec = timeoutMilliseconds / 1000;
    timeout.tv_usec = (timeoutMilliseconds % 1000) * 1000;
    const int status = select(static_cast<int>(socket) + 1, &readSet, NULL, NULL, &timeout);
    if (status > 0) {
        char buffer[512];
        const int received = recv(socket, buffer, sizeof(buffer), 0);
        if (received >= 0) {
            result.reachable = true;
            result.definitive = true;
            result.message = pingkk::text("收到 UDP 响应");
        } else {
            result.definitive = true;
            result.message = errorText(socketError());
        }
    } else if (status == 0) {
        result.message = pingkk::text("探测已发送，未收到响应");
    } else {
        result.definitive = true;
        result.message = errorText(socketError());
    }

    closeSocket(socket);
    result.elapsedMilliseconds = elapsedMilliseconds(start);
    return result;
}

std::string protocolName(Protocol protocol) {
    return protocol == Protocol::Tcp ? "TCP" : "UDP";
}

}  // namespace pingkk
