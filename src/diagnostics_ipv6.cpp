#include "diagnostics_ipv6.h"
#include "pingkk/language.h"

#ifdef __APPLE__
#define __APPLE_USE_RFC_3542
#endif

#include <chrono>
#include <cstdint>
#include <cstring>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/errqueue.h>
#endif
#endif

namespace pingkk {
namespace {

long elapsedSince(const std::chrono::steady_clock::time_point& start) {
    return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count());
}

#ifdef _WIN32
class SocketRuntime {
public:
    SocketRuntime() { WSADATA data; ready_ = WSAStartup(MAKEWORD(2, 2), &data) == 0; }
    ~SocketRuntime() { if (ready_) WSACleanup(); }
    bool ready() const { return ready_; }
private:
    bool ready_;
};
#else
// ICMPv6 errors quote the IPv6 packet, possibly with extension headers.
bool ipv6PayloadOffset(const unsigned char* bytes, std::size_t size,
                       std::size_t start, std::size_t& offset) {
    if (start + 40 > size || (bytes[start] >> 4) != 6) return false;
    unsigned int next = bytes[start + 6];
    offset = start + 40;
    while (next != IPPROTO_ICMPV6) {
        if (offset + 2 > size) return false;
        std::size_t length = 0;
        if (next == 0 || next == 43 || next == 60) {
            length = (static_cast<std::size_t>(bytes[offset + 1]) + 1) * 8;
        } else if (next == 44) {
            if (offset + 8 > size || (bytes[offset + 2] & 0xff) != 0 ||
                (bytes[offset + 3] & 0xf8) != 0) return false;
            length = 8;
        } else if (next == 51) {
            length = (static_cast<std::size_t>(bytes[offset + 1]) + 2) * 4;
        } else {
            return false;
        }
        if (offset + length > size) return false;
        next = bytes[offset];
        offset += length;
    }
    return offset + 8 <= size;
}

unsigned short read16(const unsigned char* bytes) {
    return static_cast<unsigned short>((bytes[0] << 8) | bytes[1]);
}

bool matchesReply(const unsigned char* bytes, std::size_t size,
                  const sockaddr_in6& destination, unsigned short identifier,
                  unsigned short sequence, bool datagram, int& type, int& hopLimit) {
    std::size_t offset = 0;
    if (size >= 40 && (bytes[0] >> 4) == 6) {
        hopLimit = bytes[7];
        if (!ipv6PayloadOffset(bytes, size, 0, offset)) return false;
    }
    if (offset + 8 > size) return false;
    type = bytes[offset];
    if (type == 129) {
        return bytes[offset + 1] == 0 && read16(bytes + offset + 6) == sequence &&
               (datagram || read16(bytes + offset + 4) == identifier) &&
               offset + 8 + 16 <= size && std::memcmp(bytes + offset + 8, "pingkk", 6) == 0;
    }
    if (type != 1 && type != 2 && type != 3 && type != 4) return false;
    if (type == 3 && bytes[offset + 1] != 0) return false;
    const std::size_t innerIp = offset + 8;
    std::size_t innerIcmp = 0;
    if (!ipv6PayloadOffset(bytes, size, innerIp, innerIcmp) ||
        std::memcmp(bytes + innerIp + 24, &destination.sin6_addr, 16) != 0) return false;
    return bytes[innerIcmp] == 128 && read16(bytes + innerIcmp + 6) == sequence &&
           (datagram || read16(bytes + innerIcmp + 4) == identifier);
}

#ifdef __linux__
// Linux ping sockets deliver hop-limit errors through the socket error queue.
// This keeps traceroute usable without requiring a raw-socket capability.
int readErrorQueue(int socketHandle, const sockaddr_in6& destination,
                   unsigned short sequence, PingResult& result, bool& hopExpired) {
    unsigned char packet[256];
    sockaddr_in6 original = {};
    union {
        cmsghdr align;
        unsigned char bytes[CMSG_SPACE(sizeof(sock_extended_err) + sizeof(sockaddr_in6)) + CMSG_SPACE(sizeof(int))];
    } control;
    iovec data = {packet, sizeof(packet)};
    msghdr message = {};
    message.msg_name = &original;
    message.msg_namelen = sizeof(original);
    message.msg_iov = &data;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    const ssize_t received = recvmsg(socketHandle, &message, MSG_ERRQUEUE | MSG_DONTWAIT);
    if (received < 8 || original.sin6_family != AF_INET6 ||
        std::memcmp(&original.sin6_addr, &destination.sin6_addr, 16) != 0 ||
        packet[0] != 128 || read16(packet + 6) != sequence) return -1;
    for (cmsghdr* ancillary = CMSG_FIRSTHDR(&message); ancillary != NULL;
         ancillary = CMSG_NXTHDR(&message, ancillary)) {
        if (ancillary->cmsg_level != IPPROTO_IPV6 || ancillary->cmsg_type != IPV6_RECVERR ||
            ancillary->cmsg_len < CMSG_LEN(sizeof(sock_extended_err))) continue;
        const sock_extended_err* error = reinterpret_cast<const sock_extended_err*>(CMSG_DATA(ancillary));
        if (error->ee_origin == SO_EE_ORIGIN_LOCAL) {
            result.error = std::strerror(error->ee_errno);
            return 0;
        }
        if (error->ee_origin != SO_EE_ORIGIN_ICMP6 ||
            ancillary->cmsg_len < CMSG_LEN(sizeof(sock_extended_err) + sizeof(sockaddr_in6))) continue;
        const sockaddr_in6* offender = reinterpret_cast<const sockaddr_in6*>(error + 1);
        char address[INET6_ADDRSTRLEN] = {0};
        if (offender->sin6_family == AF_INET6 &&
            inet_ntop(AF_INET6, &offender->sin6_addr, address, sizeof(address)) != NULL) {
            result.replyAddress = address;
        }
        hopExpired = error->ee_type == 3 && error->ee_code == 0;
        if (hopExpired) return 1;
        result.error = error->ee_type == 2 ? text("IPv6 数据包超过路径 MTU") : text("目标主机不可达");
        return 0;
    }
    return -1;
}
#endif
#endif

}  // namespace

bool sendIpv6Echo(const ResolvedTarget& target, int hopLimit,
                  int timeoutMilliseconds, unsigned short sequence,
                  PingResult& result, bool& hopExpired) {
    hopExpired = false;
    result.packetBytes = 64; // IPv6 header + ICMPv6 header + 16-byte payload.
#ifdef _WIN32
    SocketRuntime runtime;
    if (!runtime.ready()) {
        result.error = text("网络组件初始化失败");
        return false;
    }
#endif
    addrinfo hints = {};
    hints.ai_family = AF_INET6;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICHOST;
    addrinfo* addresses = NULL;
    if (getaddrinfo(target.ip.c_str(), NULL, &hints, &addresses) != 0 || addresses == NULL) {
        result.error = text("目标地址为空或格式不正确");
        return false;
    }
    sockaddr_in6 destination = *reinterpret_cast<const sockaddr_in6*>(addresses->ai_addr);
    freeaddrinfo(addresses);
#ifdef _WIN32
    // Windows computes the ICMPv6 checksum and handles quoted error packets.
    HANDLE handle = Icmp6CreateFile();
    if (handle == INVALID_HANDLE_VALUE) {
        result.error = text("无法创建 ICMP 探测");
        return false;
    }
    sockaddr_in6 source = {};
    source.sin6_family = AF_INET6;
    addrinfo* sources = NULL;
    const std::string localAddress = localAddressFor(target);
    if (getaddrinfo(localAddress.c_str(), NULL, &hints, &sources) != 0 || sources == NULL) {
        result.error = text("无法确定 IPv6 本机出口地址");
        IcmpCloseHandle(handle);
        return false;
    }
    source = *reinterpret_cast<const sockaddr_in6*>(sources->ai_addr);
    freeaddrinfo(sources);
    unsigned char payload[16] = {0};
    std::memcpy(payload, "pingkk", 6);
    std::vector<unsigned char> reply(sizeof(ICMPV6_ECHO_REPLY) + sizeof(payload) + 16, 0);
    IP_OPTION_INFORMATION options = {};
    options.Ttl = static_cast<unsigned char>(hopLimit);
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    const DWORD count = Icmp6SendEcho2(handle, NULL, NULL, NULL, &source, &destination,
        payload, sizeof(payload), &options, &reply[0], static_cast<DWORD>(reply.size()),
        static_cast<DWORD>(timeoutMilliseconds));
    result.elapsedMilliseconds = elapsedSince(start);
    if (count == 0) {
        result.error = GetLastError() == IP_REQ_TIMED_OUT ? text("请求超时") : text("ICMP 探测失败");
        IcmpCloseHandle(handle);
        return false;
    }
    const ICMPV6_ECHO_REPLY* echo = reinterpret_cast<const ICMPV6_ECHO_REPLY*>(&reply[0]);
    char address[INET6_ADDRSTRLEN] = {0};
    if (inet_ntop(AF_INET6, echo->Address.sin6_addr, address, sizeof(address)) != NULL) {
        result.replyAddress = address;
    }
    result.reachable = echo->Status == IP_SUCCESS;
    hopExpired = echo->Status == IP_TTL_EXPIRED_TRANSIT;
    result.elapsedMilliseconds = echo->RoundTripTime;
    if (!result.reachable && !hopExpired) {
        result.error = echo->Status == IP_REQ_TIMED_OUT ? text("请求超时") : text("目标主机不可达");
    }
    IcmpCloseHandle(handle);
    return result.reachable || hopExpired;
#else
    bool datagram = true;
    int socketHandle = socket(AF_INET6, SOCK_DGRAM, IPPROTO_ICMPV6);
    if (socketHandle < 0) {
        datagram = false;
        socketHandle = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
    }
    if (socketHandle < 0) {
        result.error = errno == EPERM || errno == EACCES
            ? text("系统不允许当前用户发送 ICMP，请授予网络诊断权限") : std::strerror(errno);
        return false;
    }
    if (setsockopt(socketHandle, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &hopLimit, sizeof(hopLimit)) != 0) {
        result.error = std::strerror(errno);
        close(socketHandle);
        return false;
    }
    const int enabled = 1;
    setsockopt(socketHandle, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &enabled, sizeof(enabled));
#ifdef __linux__
    if (datagram && setsockopt(socketHandle, IPPROTO_IPV6, IPV6_RECVERR, &enabled, sizeof(enabled)) != 0) {
        result.error = std::strerror(errno);
        close(socketHandle);
        return false;
    }
#endif
    unsigned char packet[24] = {128, 0};
    const unsigned short identifier = static_cast<unsigned short>(getpid() & 0xffff);
    packet[4] = static_cast<unsigned char>(identifier >> 8);
    packet[5] = static_cast<unsigned char>(identifier);
    packet[6] = static_cast<unsigned char>(sequence >> 8);
    packet[7] = static_cast<unsigned char>(sequence);
    std::memcpy(packet + 8, "pingkk", 6);
    // ICMPv6 sockets calculate the mandatory pseudo-header checksum in the kernel.
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    if (sendto(socketHandle, packet, sizeof(packet), 0,
               reinterpret_cast<const sockaddr*>(&destination), sizeof(destination)) < 0) {
        result.error = std::strerror(errno);
        close(socketHandle);
        return false;
    }
    while (elapsedSince(start) < timeoutMilliseconds) {
        const long remaining = timeoutMilliseconds - elapsedSince(start);
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(socketHandle, &readSet);
        timeval timeout;
        timeout.tv_sec = remaining / 1000;
        timeout.tv_usec = (remaining % 1000) * 1000;
        const int selected = select(socketHandle + 1, &readSet, NULL, NULL, &timeout);
        if (selected == 0) break;
        if (selected < 0) {
            result.error = errno == EINTR ? text("探测已中断") : std::strerror(errno);
            close(socketHandle);
            return false;
        }
        unsigned char bytes[2048];
        sockaddr_in6 source = {};
        union { cmsghdr align; unsigned char bytes[CMSG_SPACE(sizeof(int))]; } control;
        iovec data = {bytes, sizeof(bytes)};
        msghdr message = {};
        message.msg_name = &source;
        message.msg_namelen = sizeof(source);
        message.msg_iov = &data;
        message.msg_iovlen = 1;
        message.msg_control = control.bytes;
        message.msg_controllen = sizeof(control.bytes);
        const ssize_t received = recvmsg(socketHandle, &message, 0);
#ifdef __linux__
        if (received < 0 && datagram) {
            const int response = readErrorQueue(socketHandle, destination, sequence, result, hopExpired);
            if (response >= 0) {
                result.elapsedMilliseconds = elapsedSince(start);
                close(socketHandle);
                return response == 1;
            }
        }
#endif
        if (received <= 0 || (message.msg_flags & MSG_TRUNC)) continue;
        int replyHopLimit = -1;
        for (cmsghdr* ancillary = CMSG_FIRSTHDR(&message); ancillary != NULL;
             ancillary = CMSG_NXTHDR(&message, ancillary)) {
            if (ancillary->cmsg_level == IPPROTO_IPV6 && ancillary->cmsg_type == IPV6_HOPLIMIT &&
                ancillary->cmsg_len >= CMSG_LEN(sizeof(int))) {
                std::memcpy(&replyHopLimit, CMSG_DATA(ancillary), sizeof(int));
            }
        }
        int type = 0;
        if (!matchesReply(bytes, static_cast<std::size_t>(received), destination,
                          identifier, sequence, datagram, type, replyHopLimit)) continue;
        if (type == 129 && std::memcmp(&source.sin6_addr, &destination.sin6_addr, 16) != 0) continue;
        char address[INET6_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET6, &source.sin6_addr, address, sizeof(address)) != NULL) result.replyAddress = address;
        result.elapsedMilliseconds = elapsedSince(start);
        result.ttl = replyHopLimit;
        result.reachable = type == 129;
        hopExpired = type == 3;
        if (type == 1) result.error = text("目标主机不可达");
        if (type == 2) result.error = text("IPv6 数据包超过路径 MTU");
        if (type == 4) result.error = text("IPv6 数据包参数错误");
        close(socketHandle);
        return result.reachable || hopExpired;
    }
    result.elapsedMilliseconds = elapsedSince(start);
    result.error = text("请求超时");
    close(socketHandle);
    return false;
#endif
}

}  // namespace pingkk
