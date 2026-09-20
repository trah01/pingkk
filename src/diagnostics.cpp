#include "pingkk/diagnostics.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iptypes.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace pingkk {
namespace {

const std::uint8_t kIcmpEchoReply = 0;
const std::uint8_t kIcmpEchoRequest = 8;
const std::uint8_t kIcmpTimeExceeded = 11;

long elapsedSince(const std::chrono::steady_clock::time_point& start) {
    return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count());
}

#ifdef _WIN32

std::string ipv4Text(IPAddr address) {
    in_addr value;
    value.s_addr = address;
    const char* text = inet_ntoa(value);
    return text == NULL ? std::string() : std::string(text);
}

std::string icmpStatusText(IP_STATUS status) {
    switch (status) {
        case IP_SUCCESS: return std::string();
        case IP_REQ_TIMED_OUT: return "请求超时";
        case IP_TTL_EXPIRED_TRANSIT: return "TTL 已过期";
        case IP_DEST_HOST_UNREACHABLE: return "目标主机不可达";
        case IP_DEST_NET_UNREACHABLE: return "目标网络不可达";
        default: {
            std::ostringstream stream;
            stream << "ICMP 状态 " << status;
            return stream.str();
        }
    }
}

bool sendWindowsEcho(const ResolvedTarget& target,
                     int ttl,
                     int timeoutMilliseconds,
                     PingResult& result,
                     bool& ttlExpired) {
    ttlExpired = false;
    const IPAddr destination = inet_addr(target.ip.c_str());
    if (destination == INADDR_NONE) {
        result.error = "当前仅支持 IPv4 的 Ping 和路由追踪";
        return false;
    }

    HANDLE handle = IcmpCreateFile();
    if (handle == INVALID_HANDLE_VALUE) {
        result.error = "无法创建 ICMP 探测";
        return false;
    }

    const char payload[] = "pingkk";
    std::vector<unsigned char> reply(sizeof(ICMP_ECHO_REPLY) + sizeof(payload) + 16, 0);
    IP_OPTION_INFORMATION options;
    std::memset(&options, 0, sizeof(options));
    options.Ttl = static_cast<unsigned char>(ttl);
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    const DWORD count = IcmpSendEcho(handle,
                                     destination,
                                     const_cast<char*>(payload),
                                     static_cast<WORD>(sizeof(payload) - 1),
                                     &options,
                                     &reply[0],
                                     static_cast<DWORD>(reply.size()),
                                     static_cast<DWORD>(timeoutMilliseconds));
    result.elapsedMilliseconds = elapsedSince(start);
    if (count == 0) {
        const DWORD error = GetLastError();
        result.error = error == IP_REQ_TIMED_OUT ? "请求超时" : "ICMP 探测失败";
        IcmpCloseHandle(handle);
        return false;
    }

    const ICMP_ECHO_REPLY* echo = reinterpret_cast<const ICMP_ECHO_REPLY*>(&reply[0]);
    result.replyAddress = ipv4Text(echo->Address);
    result.ttl = echo->Options.Ttl;
    result.reachable = echo->Status == IP_SUCCESS;
    ttlExpired = echo->Status == IP_TTL_EXPIRED_TRANSIT;
    result.error = icmpStatusText(echo->Status);
    IcmpCloseHandle(handle);
    return result.reachable || ttlExpired;
}

#else

unsigned short checksum(const void* data, std::size_t size) {
    const unsigned short* words = static_cast<const unsigned short*>(data);
    unsigned long sum = 0;
    while (size > 1) {
        sum += *words++;
        size -= 2;
    }
    if (size == 1) sum += *reinterpret_cast<const unsigned char*>(words);
    sum = (sum >> 16) + (sum & 0xffff);
    sum += sum >> 16;
    return static_cast<unsigned short>(~sum);
}

#pragma pack(push, 1)
struct IcmpHeader {
    std::uint8_t type;
    std::uint8_t code;
    std::uint16_t checksum;
    std::uint16_t identifier;
    std::uint16_t sequence;
};

struct IcmpPacket {
    IcmpHeader header;
    unsigned char payload[16];
};
#pragma pack(pop)

bool parseIcmpReply(const unsigned char* bytes,
                    std::size_t size,
                    unsigned short sequence,
                    bool datagramSocket,
                    bool& echoReply,
                    bool& ttlExpired,
                    int& replyTtl) {
    echoReply = false;
    ttlExpired = false;
    replyTtl = -1;
    std::size_t offset = 0;
    if (size >= 20 && (bytes[0] >> 4) == 4) {
        offset = static_cast<std::size_t>(bytes[0] & 0x0f) * 4;
        replyTtl = bytes[8];
    }
    if (offset + sizeof(IcmpHeader) > size) return false;

    const IcmpHeader* icmp = reinterpret_cast<const IcmpHeader*>(bytes + offset);
    if (icmp->type == kIcmpEchoReply) {
        if (ntohs(icmp->sequence) != sequence) return false;
        echoReply = true;
        return true;
    }
    if (icmp->type != kIcmpTimeExceeded) return false;

    const std::size_t innerIpOffset = offset + 8;
    if (innerIpOffset + 20 + sizeof(IcmpHeader) > size) {
        // 某些 ICMP 数据报套接字只投递精简后的超时报文。
        ttlExpired = datagramSocket;
        return datagramSocket;
    }
    const std::size_t innerHeaderLength = static_cast<std::size_t>(bytes[innerIpOffset] & 0x0f) * 4;
    const std::size_t innerIcmpOffset = innerIpOffset + innerHeaderLength;
    if (innerIcmpOffset + sizeof(IcmpHeader) > size) return false;
    const IcmpHeader* inner = reinterpret_cast<const IcmpHeader*>(bytes + innerIcmpOffset);
    if (inner->type != kIcmpEchoRequest || ntohs(inner->sequence) != sequence) return false;
    ttlExpired = true;
    return true;
}

bool sendPosixEcho(const ResolvedTarget& target,
                   int ttl,
                   int timeoutMilliseconds,
                   unsigned short sequence,
                   PingResult& result,
                   bool& ttlExpired) {
    ttlExpired = false;
    sockaddr_in destination;
    std::memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET;
    if (inet_pton(AF_INET, target.ip.c_str(), &destination.sin_addr) != 1) {
        result.error = "当前仅支持 IPv4 的 Ping 和路由追踪";
        return false;
    }

    bool datagramSocket = true;
    int socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (socketHandle < 0) {
        datagramSocket = false;
        socketHandle = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    }
    if (socketHandle < 0) {
        result.error = errno == EPERM || errno == EACCES
                           ? "系统不允许当前用户发送 ICMP，请授予网络诊断权限"
                           : std::strerror(errno);
        return false;
    }
    if (setsockopt(socketHandle, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) != 0) {
        result.error = std::strerror(errno);
        close(socketHandle);
        return false;
    }

    IcmpPacket packet;
    std::memset(&packet, 0, sizeof(packet));
    packet.header.type = kIcmpEchoRequest;
    packet.header.identifier = htons(static_cast<unsigned short>(getpid() & 0xffff));
    packet.header.sequence = htons(sequence);
    std::memcpy(packet.payload, "pingkk", sizeof("pingkk"));
    packet.header.checksum = checksum(&packet, sizeof(packet));

    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    if (sendto(socketHandle,
               &packet,
               sizeof(packet),
               0,
               reinterpret_cast<sockaddr*>(&destination),
               sizeof(destination)) < 0) {
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
            result.error = errno == EINTR ? "探测已中断" : std::strerror(errno);
            close(socketHandle);
            return false;
        }

        unsigned char buffer[2048];
        sockaddr_in source;
        socklen_t sourceLength = sizeof(source);
        const ssize_t received = recvfrom(socketHandle,
                                          buffer,
                                          sizeof(buffer),
                                          0,
                                          reinterpret_cast<sockaddr*>(&source),
                                          &sourceLength);
        if (received <= 0) continue;
        bool echoReply = false;
        int replyTtl = -1;
        if (!parseIcmpReply(buffer,
                            static_cast<std::size_t>(received),
                            sequence,
                            datagramSocket,
                            echoReply,
                            ttlExpired,
                            replyTtl)) {
            continue;
        }
        char address[INET_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET, &source.sin_addr, address, sizeof(address)) != NULL) {
            result.replyAddress = address;
        }
        result.elapsedMilliseconds = elapsedSince(start);
        result.ttl = replyTtl;
        result.reachable = echoReply;
        close(socketHandle);
        return true;
    }

    result.elapsedMilliseconds = elapsedSince(start);
    result.error = "请求超时";
    close(socketHandle);
    return false;
}

#endif

}  // namespace

PingResult ping(const ResolvedTarget& target,
                int timeoutMilliseconds,
                unsigned short sequence) {
    PingResult result = {false, std::string(), 0, -1, std::string()};
    bool ttlExpired = false;
#ifdef _WIN32
    sendWindowsEcho(target, 64, timeoutMilliseconds, result, ttlExpired);
#else
    sendPosixEcho(target, 64, timeoutMilliseconds, sequence, result, ttlExpired);
#endif
    return result;
}

TraceHop traceHop(const ResolvedTarget& target,
                  int hop,
                  int timeoutMilliseconds,
                  unsigned short sequence) {
    PingResult pingResult = {false, std::string(), 0, -1, std::string()};
    bool ttlExpired = false;
#ifdef _WIN32
    const bool responded = sendWindowsEcho(target,
                                           hop,
                                           timeoutMilliseconds,
                                           pingResult,
                                           ttlExpired);
#else
    const bool responded = sendPosixEcho(target,
                                         hop,
                                         timeoutMilliseconds,
                                         sequence,
                                         pingResult,
                                         ttlExpired);
#endif
    TraceHop result;
    result.number = hop;
    result.responded = responded;
    result.destinationReached = pingResult.reachable;
    result.address = pingResult.replyAddress;
    result.elapsedMilliseconds = pingResult.elapsedMilliseconds;
    result.error = pingResult.error;
    return result;
}

}  // namespace pingkk
