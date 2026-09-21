#ifndef PINGKK_DIAGNOSTICS_H
#define PINGKK_DIAGNOSTICS_H

#include "pingkk/core.h"

#include <string>

namespace pingkk {

struct PingResult {
    bool reachable;
    std::string replyAddress;
    long elapsedMilliseconds;
    int ttl;
    int payloadBytes;
    int packetBytes;
    std::string error;
};

struct TraceHop {
    int number;
    bool responded;
    bool destinationReached;
    std::string address;
    long elapsedMilliseconds;
    std::string error;
};

// 发送一次 ICMP Echo 探测，不调用外部 ping 命令。
PingResult ping(const ResolvedTarget& target,
                int timeoutMilliseconds,
                unsigned short sequence);

// 使用指定 TTL 发送一次 ICMP Echo，用于逐跳路由追踪。
TraceHop traceHop(const ResolvedTarget& target,
                  int hop,
                  int timeoutMilliseconds,
                  unsigned short sequence);

}  // namespace pingkk

#endif
