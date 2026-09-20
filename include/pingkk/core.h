#ifndef PINGKK_CORE_H
#define PINGKK_CORE_H

#include <string>
#include <vector>

namespace pingkk {

enum class Protocol {
    Tcp,
    Udp
};

struct ResolvedTarget {
    std::string input;
    std::string host;
    std::string ip;
};

struct ProbeResult {
    Protocol protocol;
    bool reachable;
    bool definitive;
    long elapsedMilliseconds;
    std::string message;
};

// 从域名、IP 或 URL 中提取可用于网络测试的主机名。
std::string extractHost(const std::string& input);

// 将主机名解析为 IP 地址，优先返回 IPv4。
bool resolveTarget(const std::string& input,
                   ResolvedTarget& target,
                   std::string& error);

// 获取访问目标时操作系统选择的本机 IP。
std::string localAddressFor(const ResolvedTarget& target);

// 获取默认网络出口使用的本机 IP，无需先输入测试目标。
std::string primaryLocalAddress();

// 获取系统当前配置的 DNS 服务器地址。
std::vector<std::string> currentDnsServers();

ProbeResult probeTcp(const ResolvedTarget& target,
                     unsigned short port,
                     int timeoutMilliseconds);

ProbeResult probeUdp(const ResolvedTarget& target,
                     unsigned short port,
                     int timeoutMilliseconds);

std::string protocolName(Protocol protocol);

}  // namespace pingkk

#endif
