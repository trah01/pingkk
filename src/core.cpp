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

std::string errorText(int error) {
#ifdef _WIN32
    std::ostringstream stream;
    stream << "系统错误 " << error;
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
                   std::string& error) {
    target.input = input;
    target.host = extractHost(input);
    target.ip.clear();
    if (target.host.empty()) {
        error = "目标地址为空或格式不正确";
        return false;
    }

    SocketRuntime runtime;
    if (!runtime.ready()) {
        error = "网络组件初始化失败";
        return false;
    }

    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = NULL;
    const int status = getaddrinfo(target.host.c_str(), NULL, &hints, &results);
    if (status != 0 || results == NULL) {
#ifdef _WIN32
        error = "无法解析目标地址";
#else
        error = gai_strerror(status);
#endif
        return false;
    }

    const addrinfo* selected = results;
    for (const addrinfo* current = results; current != NULL; current = current->ai_next) {
        if (current->ai_family == AF_INET) {
            selected = current;
            break;
        }
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
        error = "解析结果中没有可用的 IP 地址";
        return false;
    }
    return true;
}

std::string localAddressFor(const ResolvedTarget& target) {
    SocketRuntime runtime;
    sockaddr_storage remote;
    AddressLength remoteLength = 0;
    if (!runtime.ready() || !makeAddress(target, 53, remote, remoteLength)) {
        return "未知";
    }

    const int family = target.ip.find(':') == std::string::npos ? AF_INET : AF_INET6;
    SocketHandle socket = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == kInvalidSocket) {
        return "未知";
    }
    if (::connect(socket, reinterpret_cast<sockaddr*>(&remote), remoteLength) != 0) {
        closeSocket(socket);
        return "未知";
    }

    sockaddr_storage local;
    AddressLength localLength = sizeof(local);
    std::memset(&local, 0, sizeof(local));
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&local), &localLength) != 0) {
        closeSocket(socket);
        return "未知";
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
               : "未知";
}

std::string primaryLocalAddress() {
    ResolvedTarget routeTarget;
    routeTarget.input = "223.5.5.5";
    routeTarget.host = "223.5.5.5";
    routeTarget.ip = "223.5.5.5";
    return localAddressFor(routeTarget);
}

std::vector<std::string> currentDnsServers() {
    std::vector<std::string> servers;
#ifdef _WIN32
    ULONG size = 0;
    if (GetNetworkParams(NULL, &size) != ERROR_BUFFER_OVERFLOW || size == 0) {
        return servers;
    }
    std::vector<unsigned char> buffer(size, 0);
    FIXED_INFO* information = reinterpret_cast<FIXED_INFO*>(&buffer[0]);
    if (GetNetworkParams(information, &size) != ERROR_SUCCESS) return servers;
    for (IP_ADDR_STRING* entry = &information->DnsServerList;
         entry != NULL;
         entry = entry->Next) {
        appendUnique(servers, entry->IpAddress.String);
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
        result.message = "无法创建目标地址";
        return result;
    }

    const int family = target.ip.find(':') == std::string::npos ? AF_INET : AF_INET6;
    SocketHandle socket = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (socket == kInvalidSocket || !setNonBlocking(socket, true)) {
        result.message = "无法创建 TCP 连接";
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
                result.message = result.reachable ? "连接成功" : errorText(pendingError);
            }
        } else {
            result.message = status == 0 ? "连接超时" : errorText(socketError());
        }
    } else {
        result.reachable = true;
        result.message = "连接成功";
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
        result.message = "无法创建目标地址";
        return result;
    }

    const int family = target.ip.find(':') == std::string::npos ? AF_INET : AF_INET6;
    SocketHandle socket = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == kInvalidSocket) {
        result.message = "无法创建 UDP 探测";
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
            result.message = "收到 UDP 响应";
        } else {
            result.definitive = true;
            result.message = errorText(socketError());
        }
    } else if (status == 0) {
        result.message = "探测已发送，未收到响应";
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
