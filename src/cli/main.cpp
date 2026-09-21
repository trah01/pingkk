#include "pingkk/core.h"
#include "pingkk/diagnostics.h"
#include "pingkk/network_check.h"
#include "pingkk/version.h"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

volatile std::sig_atomic_t g_running = 1;

void stopRunning(int) {
    g_running = 0;
}

void waitUntilOrStopped(const std::chrono::steady_clock::time_point& deadline) {
    while (g_running) {
        const std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now();
        if (now >= deadline) return;
        const std::chrono::milliseconds remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        std::this_thread::sleep_for(
            std::min(remaining, std::chrono::milliseconds(50)));
    }
}

void printCurrentDns() {
    const std::vector<std::string> servers = pingkk::currentDnsServers();
    if (servers.empty()) {
        std::cout << "当前 DNS：未检测到\n";
        return;
    }
    std::cout << "当前 DNS：";
    for (std::size_t index = 0; index < servers.size(); ++index) {
        if (index > 0) std::cout << ", ";
        std::cout << servers[index];
    }
    std::cout << "\n";
}

void printHelp() {
    std::cout
        << "ping看看（pingkk）v" PINGKK_VERSION " 端口连通性测试工具\n"
        << "项目地址：" PINGKK_PROJECT_URL "\n\n"
        << "用法：\n"
        << "  pingkk <地址> <端口> [tcp|udp|all]   测试一次端口\n"
        << "  pingkk -t <地址> <端口> [协议]       每秒持续测试\n"
        << "  pingkk <地址> -t <端口> [协议]       每秒持续测试\n"
        << "  pingkk <地址>                         执行 Ping 测试\n"
        << "  pingkk -r <地址>                      路由追踪（不解析节点域名）\n"
        << "  pingkk --checkup                     一键网络体检\n"
        << "  pingkk --dns <域名>                  对比系统、当前及公共 DNS\n"
        << "  -a, --advanced                       输出更详细的诊断信息\n"
        << "  --timeout <毫秒>                        设置超时时间，默认 1000\n"
        << "  pingkk gui                            打开图形界面\n"
        << "  pingkk -h | --help                    显示本教程\n\n"
        << "选项 -t、-r、--route、-a、--advanced、--timeout 和 -w 可以放在命令中的任意位置。\n"
        << "一次只能测试一个地址；多个端口使用英文逗号分隔。\n"
        << "地址可以填写 IP、域名或完整网址，例如：https://example.com/path\n"
        << "UDP 只有收到目标响应时才能确认连通；无响应不等于端口不通。\n";
}

std::string joinAddresses(const std::vector<std::string>& addresses) {
    if (addresses.empty()) return "—";
    std::ostringstream text;
    for (std::size_t index = 0; index < addresses.size(); ++index) {
        if (index > 0) text << ", ";
        text << addresses[index];
    }
    return text.str();
}

void printAdvancedTarget(const pingkk::ResolvedTarget& target,
                         const std::string& mode,
                         int timeoutMilliseconds,
                         bool includeLocalAddress) {
    std::cout << "[高级信息]\n"
              << "  原始输入：" << target.input << "\n"
              << "  解析主机：" << target.host << "\n"
              << "  目标 IP：" << target.ip << "\n"
              << "  地址类型：" << (target.ip.find(':') == std::string::npos ? "IPv4" : "IPv6") << "\n";
    if (includeLocalAddress) {
        std::cout << "  本机出口 IP：" << pingkk::localAddressFor(target) << "\n";
    }
    std::cout << "  测试模式：" << mode << "\n"
              << "  超时时间：" << timeoutMilliseconds << " 毫秒\n";
}

void printTimingStatistics(const std::string& title,
                           int sent,
                           int received,
                           const std::vector<long>& elapsedTimes) {
    const int lost = sent - received;
    const double lossRate = sent == 0 ? 0.0 : 100.0 * lost / sent;
    std::cout << "[高级统计] " << title << "\n"
              << "  已发送：" << sent << "，已响应：" << received
              << "，丢失：" << lost << "（" << std::fixed << std::setprecision(1)
              << lossRate << "%）\n" << std::defaultfloat;
    if (elapsedTimes.empty()) return;

    const long minimum = *std::min_element(elapsedTimes.begin(), elapsedTimes.end());
    const long maximum = *std::max_element(elapsedTimes.begin(), elapsedTimes.end());
    double total = 0.0;
    for (std::size_t index = 0; index < elapsedTimes.size(); ++index) {
        total += elapsedTimes[index];
    }
    const double average = total / elapsedTimes.size();
    double squaredDeviation = 0.0;
    for (std::size_t index = 0; index < elapsedTimes.size(); ++index) {
        const double difference = elapsedTimes[index] - average;
        squaredDeviation += difference * difference;
    }
    const double variation = std::sqrt(squaredDeviation / elapsedTimes.size());
    std::cout << std::fixed << std::setprecision(1)
              << "  往返时延：最小 " << minimum << " 毫秒，最大 " << maximum
              << " 毫秒，平均 " << average << " 毫秒，波动 " << variation << " 毫秒\n";
    std::cout << std::defaultfloat;
}

void printProbeStatistics(int attempts,
                          int reachable,
                          int failed,
                          int unknown,
                          const std::vector<long>& successfulTimes) {
    std::cout << "[高级统计] 端口测试汇总\n"
              << "  总次数：" << attempts << "，连通：" << reachable
              << "，不通：" << failed << "，状态未知：" << unknown << "\n";
    if (successfulTimes.empty()) return;
    const long minimum = *std::min_element(successfulTimes.begin(), successfulTimes.end());
    const long maximum = *std::max_element(successfulTimes.begin(), successfulTimes.end());
    double total = 0.0;
    for (std::size_t index = 0; index < successfulTimes.size(); ++index) {
        total += successfulTimes[index];
    }
    const double average = total / successfulTimes.size();
    double squaredDeviation = 0.0;
    for (std::size_t index = 0; index < successfulTimes.size(); ++index) {
        const double difference = successfulTimes[index] - average;
        squaredDeviation += difference * difference;
    }
    const double variation = std::sqrt(squaredDeviation / successfulTimes.size());
    std::cout << std::fixed << std::setprecision(1)
              << "  连通耗时：最小 " << minimum << " 毫秒，最大 " << maximum
              << " 毫秒，平均 " << average << " 毫秒，波动 " << variation << " 毫秒\n"
              << std::defaultfloat;
}

int printDnsComparison(const std::string& input,
                       int timeoutMilliseconds,
                       bool heading,
                       bool advanced) {
    const std::string host = pingkk::extractHost(input);
    if (host.empty()) {
        std::cerr << "域名格式不正确。\n";
        return 2;
    }
    if (heading) std::cout << "正在对比 " << host << " 的 DNS 解析结果：\n";
    if (advanced) {
        std::cout << "[高级信息]\n"
                  << "  查询域名：" << host << "\n"
                  << "  查询记录：A、AAAA\n"
                  << "  单次超时：" << timeoutMilliseconds << " 毫秒\n"
                  << "  对比来源：系统 DNS、当前 DNS、阿里 DNS、腾讯 DNS\n";
    }
    const std::vector<pingkk::DnsLookupResult> results =
        pingkk::compareDns(host, timeoutMilliseconds);
    if (results.empty()) {
        std::cerr << "没有可用的 DNS 服务器。\n";
        return 2;
    }

    std::set<std::string> distinctResults;
    int successfulResolvers = 0;
    int failedResolvers = 0;
    int publicResolvers = 0;
    int publicNameDoesNotExist = 0;
    int publicNoAddressRecords = 0;
    bool localResolverSucceeded = false;
    std::vector<long> dnsElapsedTimes;
    for (std::size_t index = 0; index < results.size(); ++index) {
        const pingkk::DnsLookupResult& result = results[index];
        dnsElapsedTimes.push_back(result.elapsedMilliseconds);
        std::cout << (result.success ? "[正常] " : "[失败] ")
                  << result.resolverName << "（" << result.resolverAddress << "）";
        if (!result.success) {
            ++failedResolvers;
            if (result.resolverName == "阿里 DNS" || result.resolverName == "腾讯 DNS") {
                ++publicResolvers;
                if (result.nameDoesNotExist) ++publicNameDoesNotExist;
                if (result.noAddressRecords) ++publicNoAddressRecords;
            }
            std::cout << "：" << result.error << "，" << result.elapsedMilliseconds << "毫秒\n";
            continue;
        }
        if (result.resolverName == "阿里 DNS" || result.resolverName == "腾讯 DNS") {
            ++publicResolvers;
        } else {
            localResolverSucceeded = true;
        }
        std::cout << "：" << result.elapsedMilliseconds << "毫秒\n"
                  << "  IPv4：" << joinAddresses(result.ipv4Addresses) << "\n"
                  << "  IPv6：" << joinAddresses(result.ipv6Addresses) << "\n";
        std::vector<std::string> combined = result.ipv4Addresses;
        ++successfulResolvers;
        combined.insert(combined.end(), result.ipv6Addresses.begin(), result.ipv6Addresses.end());
        std::sort(combined.begin(), combined.end());
        distinctResults.insert(joinAddresses(combined));
    }
    if (advanced) {
        double totalElapsed = 0.0;
        for (std::size_t index = 0; index < dnsElapsedTimes.size(); ++index) {
            totalElapsed += dnsElapsedTimes[index];
        }
        const long minimum = *std::min_element(dnsElapsedTimes.begin(), dnsElapsedTimes.end());
        const long maximum = *std::max_element(dnsElapsedTimes.begin(), dnsElapsedTimes.end());
        std::cout << "[高级统计] DNS 查询汇总\n"
                  << "  解析器：" << results.size() << "，成功：" << successfulResolvers
                  << "，失败：" << failedResolvers
                  << "，公共 DNS 返回 NXDOMAIN：" << publicNameDoesNotExist
                  << "，无 A/AAAA 记录：" << publicNoAddressRecords << "\n"
                  << std::fixed << std::setprecision(1)
                  << "  查询耗时：最小 " << minimum << " 毫秒，最大 " << maximum
                  << " 毫秒，平均 " << totalElapsed / dnsElapsedTimes.size() << " 毫秒\n"
                  << std::defaultfloat;
    }
    if (localResolverSucceeded && publicResolvers > 0 &&
        publicNameDoesNotExist == publicResolvers) {
        std::cout << "结论：DNS 解析结果存在差异。系统或当前 DNS 可以解析，"
                  << "但公共 DNS 返回域名不存在（NXDOMAIN）。可能原因包括内网或分区 DNS、"
                  << "DNS 拦截/过滤，或公网记录尚未同步；仅凭本次检测无法断定该域名只能在当前网络使用。\n";
    } else if (publicResolvers > 0 &&
               publicNoAddressRecords == publicResolvers) {
        std::cout << "结论：公共 DNS 均未返回 A 或 AAAA 记录。域名可能存在，"
                  << "但当前没有配置可用于访问的 IPv4 或 IPv6 地址。\n";
    } else if (successfulResolvers == 0) {
        std::cout << "结论：所有 DNS 查询均失败，请检查网络、防火墙或 DNS 设置。\n";
    } else if (failedResolvers > 0) {
        std::cout << "结论：部分 DNS 查询失败，不能判定结果一致；"
                  << successfulResolvers << " 个解析器成功，"
                  << failedResolvers << " 个失败。\n";
    } else if (distinctResults.size() > 1) {
        std::cout << "结论：不同 DNS 返回的 IP 不完全一致；CDN 调度可能导致差异，如访问异常请优先检查系统 DNS。\n";
    } else {
        std::cout << "结论：各 DNS 返回结果一致。\n";
    }
    return results[0].success ? 0 : 1;
}

int runCheckup(int timeoutMilliseconds, bool advanced) {
    const std::chrono::steady_clock::time_point checkupStart =
        std::chrono::steady_clock::now();
    std::cout << "开始一键网络体检……\n\n";
    if (advanced) {
        std::cout << "[高级信息]\n"
                  << "  基准域名：www.baidu.com\n"
                  << "  HTTPS 目标端口：443\n"
                  << "  单项超时：" << timeoutMilliseconds << " 毫秒\n\n";
    }
    bool healthy = true;

    const std::string localAddress = pingkk::primaryLocalAddress();
    std::cout << "[1/5] 本机网络\n"
              << "  当前 IP：" << localAddress << "\n"
              << "  默认网关：" << pingkk::defaultGateway() << "\n";
    if (localAddress == "未知") healthy = false;

    std::cout << "[2/5] DNS 配置\n";
    const std::vector<std::string> dnsServers = pingkk::currentDnsServers();
    std::cout << "  当前 DNS：" << joinAddresses(dnsServers) << "\n";
    if (dnsServers.empty()) healthy = false;

    std::cout << "[3/5] 代理设置\n";
    const std::vector<std::string> proxies = pingkk::currentProxySettings();
    if (proxies.empty()) {
        std::cout << "  未检测到已启用的代理。\n";
    } else {
        for (std::size_t index = 0; index < proxies.size(); ++index) {
            std::cout << "  " << proxies[index] << "\n";
        }
    }

    std::cout << "[4/5] 域名解析\n";
    pingkk::ResolvedTarget checkTarget;
    std::string resolveError;
    const std::chrono::steady_clock::time_point resolveStart =
        std::chrono::steady_clock::now();
    const bool resolved = pingkk::resolveTarget(
        "www.baidu.com", checkTarget, resolveError);
    const long resolveElapsed = static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - resolveStart).count());
    if (!resolved) {
        std::cout << "  [失败] 系统 DNS 无法解析 www.baidu.com："
                  << resolveError << "\n";
        healthy = false;
    } else {
        std::cout << "  [正常] 系统 DNS：" << resolveElapsed
                  << "毫秒，IP " << checkTarget.ip << "\n";
    }

    std::cout << "[5/5] HTTPS 直连\n";
    if (resolved) {
        const pingkk::ProbeResult probe = pingkk::probeTcp(
            checkTarget, 443, timeoutMilliseconds);
        std::cout << "  " << (probe.reachable ? "[正常] " : "[失败] ")
                  << checkTarget.ip << ":443 " << probe.message
                  << "，" << probe.elapsedMilliseconds << "毫秒\n";
        if (!probe.reachable) healthy = false;
    } else {
        std::cout << "  [跳过] 没有可用的 IPv4 解析结果。\n";
        healthy = false;
    }

    if (advanced) {
        const long totalElapsed = static_cast<long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - checkupStart).count());
        std::cout << "[高级统计] 体检汇总\n"
                  << "  DNS 服务器：" << dnsServers.size()
                  << "，已启用代理：" << proxies.size() << "\n"
                  << "  DNS 解析耗时：" << resolveElapsed << " 毫秒\n"
                  << "  总耗时：" << totalElapsed << " 毫秒\n";
    }

    std::cout << "\n体检结论："
              << (healthy ? "基础网络状态正常。" : "发现异常，请根据上方失败项目继续排查。")
              << "\n";
    return healthy ? 0 : 1;
}

int runSystemTool(const std::vector<std::string>& arguments) {
#ifdef _WIN32
    std::ostringstream command;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (index) command << ' ';
        command << '"' << arguments[index] << '"';
    }
    return std::system(command.str().c_str());
#else
    std::vector<char*> raw;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        raw.push_back(const_cast<char*>(arguments[index].c_str()));
    }
    raw.push_back(NULL);
    const pid_t child = fork();
    if (child == 0) {
        execvp(raw[0], &raw[0]);
        _exit(127);
    }
    if (child < 0) return 1;
    int status = 0;
    waitpid(child, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

int runPingOrRoute(const std::string& input,
                   bool route,
                   bool continuous,
                   int timeoutMilliseconds,
                   bool advanced) {
    const std::string host = pingkk::extractHost(input);
    if (host.empty()) {
        std::cerr << "目标地址格式不正确。\n";
        return 2;
    }

    pingkk::ResolvedTarget target;
    std::string error;
    if (!pingkk::resolveTarget(host, target, error)) {
        std::cerr << "无法解析 " << host << "：" << error << "\n";
        printCurrentDns();
        return 2;
    }
    if (advanced) {
        printAdvancedTarget(
            target,
            route ? "路由追踪" : (continuous ? "持续 Ping" : "Ping"),
            timeoutMilliseconds,
            true);
    }
    if (target.host != target.ip) {
        std::cout << "域名 " << target.host << " 解析为 IP " << target.ip << "\n";
    }
    printCurrentDns();

    if (route) {
        const std::chrono::steady_clock::time_point routeStart =
            std::chrono::steady_clock::now();
        int attemptedHops = 0;
        int respondingHops = 0;
        const auto printRouteSummary = [&]() {
            if (!advanced) return;
            const long totalElapsed = static_cast<long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - routeStart).count());
            std::cout << "[高级统计] 路由追踪汇总\n"
                      << "  已探测跳数：" << attemptedHops
                      << "，有响应：" << respondingHops
                      << "，无响应：" << attemptedHops - respondingHops << "\n"
                      << "  总耗时：" << totalElapsed << " 毫秒\n";
        };
        std::cout << "正在追踪到 " << target.ip << " 的路由，最多 30 跳：\n";
        for (int hop = 1; hop <= 30 && g_running; ++hop) {
            const pingkk::TraceHop result = pingkk::traceHop(
                target, hop, timeoutMilliseconds, static_cast<unsigned short>(hop));
            ++attemptedHops;
            if (!g_running) break;
            std::cout << ' ' << hop << "  ";
            if (!result.responded) {
                std::cout << "*";
                if (!result.error.empty() && result.error != "请求超时") {
                    std::cout << "  " << result.error << "\n";
                    printRouteSummary();
                    return 1;
                }
            } else {
                ++respondingHops;
                std::cout << result.address << "  " << result.elapsedMilliseconds << "毫秒";
                if (advanced) std::cout << "  TTL=" << hop;
            }
            std::cout << "\n";
            if (result.destinationReached) {
                std::cout << "路由追踪完成。\n";
                printRouteSummary();
                return 0;
            }
        }
        if (!g_running) {
            printRouteSummary();
            return 130;
        }
        std::cout << "已达到最大跳数，目标尚未响应。\n";
        printRouteSummary();
        return 1;
    }

    bool anyReply = false;
    bool fatalError = false;
    int sentPackets = 0;
    int receivedPackets = 0;
    std::vector<long> roundTripTimes;
    std::cout << "正在 Ping " << target.ip << "：\n";
    unsigned short sequence = 1;
    do {
        const std::chrono::steady_clock::time_point iterationStart =
            std::chrono::steady_clock::now();
        ++sentPackets;
        const pingkk::PingResult result = pingkk::ping(
            target, timeoutMilliseconds, sequence);
        if (result.reachable) {
            anyReply = true;
            ++receivedPackets;
            roundTripTimes.push_back(result.elapsedMilliseconds);
            std::cout << "来自 " << result.replyAddress << "：时间="
                      << result.elapsedMilliseconds << "毫秒";
            if (result.ttl >= 0) std::cout << " TTL=" << result.ttl;
            if (advanced) {
                std::cout << " 序号=" << sequence
                          << " 数据=" << result.payloadBytes << "字节"
                          << " IP包=" << result.packetBytes << "字节";
            }
            std::cout << "\n";
        } else {
            if (advanced) {
                std::cout << "序号=" << sequence << " " << result.error
                          << " 数据=" << result.payloadBytes << "字节"
                          << " IP包=" << result.packetBytes << "字节\n";
            } else {
                std::cout << result.error << "\n";
            }
            if (!result.error.empty() && result.error != "请求超时" &&
                result.error != "探测已中断") {
                fatalError = true;
            }
        }
        if (!g_running || fatalError) break;
        ++sequence;
        const bool hasNextProbe = g_running && (continuous || sequence <= 4);
        if (hasNextProbe) {
            waitUntilOrStopped(iterationStart + std::chrono::seconds(1));
        }
    } while (g_running && (continuous || sequence <= 4));
    if (advanced) {
        printTimingStatistics("Ping 汇总", sentPackets, receivedPackets, roundTripTimes);
        std::cout << "  ICMP 数据：" << 16 << " 字节，IPv4 包：" << 44
                  << " 字节（不含链路层开销）\n";
    }
    if (!g_running) return 130;
    if (fatalError) return 1;
    return anyReply ? 0 : 1;
}

bool parsePort(const std::string& value, unsigned short& port) {
    char* end = NULL;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed < 1 || parsed > 65535) {
        return false;
    }
    port = static_cast<unsigned short>(parsed);
    return true;
}

std::string trim(const std::string& value) {
    const std::string whitespace = " \t\r\n";
    const std::string::size_type start = value.find_first_not_of(whitespace);
    if (start == std::string::npos) return std::string();
    const std::string::size_type end = value.find_last_not_of(whitespace);
    return value.substr(start, end - start + 1);
}

bool splitCommaSeparated(const std::string& value,
                         std::vector<std::string>& items) {
    std::string::size_type start = 0;
    while (start <= value.size()) {
        const std::string::size_type comma = value.find(',', start);
        const std::string item = trim(value.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start));
        if (item.empty()) return false;
        items.push_back(item);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return !items.empty();
}

bool parsePorts(const std::string& value,
                std::vector<unsigned short>& ports) {
    std::vector<std::string> values;
    if (!splitCommaSeparated(value, values)) return false;
    for (std::size_t index = 0; index < values.size(); ++index) {
        unsigned short port = 0;
        if (!parsePort(values[index], port)) return false;
        ports.push_back(port);
    }
    return true;
}

bool parseTimeout(const std::string& value, int& timeoutMilliseconds) {
    char* end = NULL;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed < 100 || parsed > 60000) {
        return false;
    }
    timeoutMilliseconds = static_cast<int>(parsed);
    return true;
}

void printProbe(const pingkk::ResolvedTarget& target,
                unsigned short port,
                const pingkk::ProbeResult& result) {
    std::cout << "对 " << target.ip << " 的 " << port << " 端口测试"
              << "........" << pingkk::protocolName(result.protocol) << " 协议";
    if (result.reachable) {
        std::cout << "连通";
    } else if (!result.definitive) {
        std::cout << "状态未知";
    } else {
        std::cout << "不通";
    }
    std::cout << "（" << result.message << "，" << result.elapsedMilliseconds << "毫秒）\n";
}

int launchGui(const char* executablePath) {
#ifdef __APPLE__
    (void)executablePath;
    return runSystemTool(std::vector<std::string>{"open", "-a", "pingkk"});
#elif defined(_WIN32)
    std::string path(executablePath);
    const std::string::size_type slash = path.find_last_of("/\\");
    path = (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) + "pingkk-gui.exe";
    return runSystemTool(std::vector<std::string>{path});
#else
    std::string path(executablePath);
    const std::string::size_type slash = path.find_last_of('/');
    path = (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) + "pingkk-gui";
    return runSystemTool(std::vector<std::string>{path});
#endif
}

}  // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);
    std::signal(SIGINT, stopRunning);
    std::signal(SIGTERM, stopRunning);
    if (argc == 1) {
        printHelp();
        return 0;
    }

    std::vector<std::string> arguments(argv + 1, argv + argc);
    if (arguments[0] == "-h" || arguments[0] == "--help") {
        printHelp();
        return 0;
    }
    if (arguments[0] == "gui") {
        return launchGui(argv[0]);
    }

    int timeoutMilliseconds = 1000;
    for (std::size_t index = 0; index < arguments.size();) {
        if (arguments[index] != "--timeout" && arguments[index] != "-w") {
            ++index;
            continue;
        }
        if (index + 1 >= arguments.size() ||
            !parseTimeout(arguments[index + 1], timeoutMilliseconds)) {
            std::cerr << "超时时间必须是 100 到 60000 之间的毫秒数。\n";
            return 2;
        }
        arguments.erase(arguments.begin() + index, arguments.begin() + index + 2);
    }
    bool continuous = false;
    bool route = false;
    bool advanced = false;
    for (std::vector<std::string>::iterator it = arguments.begin(); it != arguments.end();) {
        if (*it == "-t") {
            continuous = true;
            it = arguments.erase(it);
        } else if (*it == "-r" || *it == "--route") {
            route = true;
            it = arguments.erase(it);
        } else if (*it == "-a" || *it == "--advanced") {
            advanced = true;
            it = arguments.erase(it);
        } else {
            ++it;
        }
    }
    if (arguments.empty()) {
        std::cerr << "缺少目标地址。\n";
        return 2;
    }

    if (arguments[0] == "--checkup") {
        if (arguments.size() != 1) {
            std::cerr << "一键体检不需要目标地址。\n";
            return 2;
        }
        return runCheckup(timeoutMilliseconds, advanced);
    }
    if (arguments[0] == "--dns") {
        if (arguments.size() != 2) {
            std::cerr << "DNS 对比需要一个域名。\n";
            return 2;
        }
        return printDnsComparison(arguments[1], timeoutMilliseconds, true, advanced);
    }

    if (pingkk::extractHost(arguments[0]).find(',') != std::string::npos) {
        std::cerr << "一次只能测试一个目标地址。\n";
        return 2;
    }

    if (route) {
        if (continuous) {
            std::cerr << "持续测试不能与路由追踪同时使用。\n";
            return 2;
        }
        if (arguments.size() != 1) {
            std::cerr << "路由追踪只需要目标地址。\n";
            return 2;
        }
        return runPingOrRoute(arguments[0], true, false, timeoutMilliseconds, advanced);
    }

    if (arguments.size() == 1) {
        return runPingOrRoute(arguments[0], false, continuous, timeoutMilliseconds, advanced);
    }
    if (arguments.size() < 2 || arguments.size() > 3) {
        std::cerr << "参数数量不正确，请运行 pingkk --help 查看用法。\n";
        return 2;
    }

    std::vector<unsigned short> ports;
    if (!parsePorts(arguments[1], ports)) {
        std::cerr << "端口必须是 1 到 65535 之间的数字，多个端口请使用英文逗号分隔。\n";
        return 2;
    }
    const std::string protocol = arguments.size() == 3 ? arguments[2] : "tcp";
    if (protocol != "tcp" && protocol != "udp" && protocol != "all") {
        std::cerr << "协议只能是 tcp、udp 或 all。\n";
        return 2;
    }

    pingkk::ResolvedTarget target;
    std::string error;
    if (!pingkk::resolveTarget(arguments[0], target, error)) {
        std::cerr << "无法解析目标地址：" << error << "\n";
        printCurrentDns();
        return 2;
    }
    if (advanced) {
        std::string mode = protocol == "tcp" ? "TCP 端口测试" :
                           protocol == "udp" ? "UDP 端口测试" :
                                               "TCP + UDP 端口测试";
        if (continuous) mode = "持续" + mode;
        printAdvancedTarget(target, mode, timeoutMilliseconds, false);
        std::cout << "  目标端口：";
        for (std::size_t index = 0; index < ports.size(); ++index) {
            if (index > 0) std::cout << ", ";
            std::cout << ports[index];
        }
        std::cout << "\n";
    }
    std::cout << "当前 IP：" << pingkk::localAddressFor(target) << "\n";
    if (target.host != target.ip) {
        std::cout << "域名 " << target.host << " 解析为 IP " << target.ip << "\n";
    }
    printCurrentDns();

    bool allReachable = true;
    int probeAttempts = 0;
    int reachableProbes = 0;
    int failedProbes = 0;
    int unknownProbes = 0;
    std::vector<long> successfulProbeTimes;
    const auto recordProbe = [&](const pingkk::ProbeResult& result) {
        ++probeAttempts;
        if (result.reachable) {
            ++reachableProbes;
            successfulProbeTimes.push_back(result.elapsedMilliseconds);
        } else if (result.definitive) {
            ++failedProbes;
        } else {
            ++unknownProbes;
        }
    };
    do {
        const std::chrono::steady_clock::time_point iterationStart =
            std::chrono::steady_clock::now();
        for (std::size_t portIndex = 0;
             portIndex < ports.size() && g_running;
             ++portIndex) {
            if (protocol == "tcp" || protocol == "all") {
                const pingkk::ProbeResult result = pingkk::probeTcp(
                    target, ports[portIndex], timeoutMilliseconds);
                printProbe(target, ports[portIndex], result);
                recordProbe(result);
                if (!result.reachable) allReachable = false;
            }
            if ((protocol == "udp" || protocol == "all") && g_running) {
                const pingkk::ProbeResult result = pingkk::probeUdp(
                    target, ports[portIndex], timeoutMilliseconds);
                printProbe(target, ports[portIndex], result);
                recordProbe(result);
                if (!result.reachable) allReachable = false;
            }
        }
        if (continuous && g_running) {
            waitUntilOrStopped(iterationStart + std::chrono::seconds(1));
        }
    } while (continuous && g_running);

    if (advanced) {
        printProbeStatistics(probeAttempts, reachableProbes, failedProbes,
                             unknownProbes, successfulProbeTimes);
    }
    if (!g_running) return 130;
    return allReachable ? 0 : 1;
}
