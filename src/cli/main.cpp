#include "arguments.h"
#include "pingkk/language.h"
#include "pingkk/core.h"
#include "pingkk/diagnostics.h"
#include "pingkk/network_check.h"
#include "pingkk/version.h"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cerrno>
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
#define NOMINMAX
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <CoreServices/CoreServices.h>
#endif

namespace {

using pingkk::cli::parseTestArguments;
using pingkk::cli::parseTimeout;

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
        std::cout << pingkk::text("当前 DNS：未检测到\n");
        return;
    }
    std::cout << pingkk::text("当前 DNS：");
    for (std::size_t index = 0; index < servers.size(); ++index) {
        if (index > 0) std::cout << ", ";
        std::cout << servers[index];
    }
    std::cout << "\n";
}

void printHelp() {
    std::cout
        << pingkk::text("ping看看（pingkk）v") << PINGKK_VERSION << pingkk::text(" 端口连通性测试工具\n")
        << pingkk::text("项目地址：") << PINGKK_PROJECT_URL << "\n\n"
        << pingkk::text("用法：\n")
        << pingkk::text("  完整请求：\n")
        << pingkk::text("  pingkk <地址> <端口> [tcp|udp|all]   测试一次端口\n")
        << pingkk::text("  pingkk <地址>                      执行 Ping 测试 4 次\n")
        << pingkk::text("  pingkk -r, --route <地址>          路由追踪（不解析节点域名）\n")
        << pingkk::text("  pingkk -jc, --checkup              一键网络检查\n")
        << pingkk::text("  pingkk --dns <域名>                对比系统、当前及公共 DNS\n")
        << pingkk::text("  额外附加参数：\n")
        << pingkk::text("    -a, --advanced                  输出更详细的诊断信息\n")
        << pingkk::text("    --timeout <毫秒>                设置超时时间，默认 1000\n")
        << pingkk::text("    -t                              持续测试\n")
        << pingkk::text("    -v6                             使用 IPv6，默认 IPv4\n")
        << pingkk::text("    -v46                            同时测试 IPv4 和 IPv6\n")
        << pingkk::text("    --lang <zh|en>                  设置输出语言，默认 zh\n")
        << pingkk::text("  pingkk gui                        打开图形界面\n")
        << pingkk::text("  pingkk -h | --help                 显示本教程\n\n")
        << pingkk::text("额外附加参数可以放在命令中的任意位置；--timeout 和 --lang 的值需紧跟对应选项。\n")
        << pingkk::text("一次只能测试一个地址；多个端口使用英文逗号分隔。\n")
        << pingkk::text("地址可以填写 IP、域名或完整网址，例如：https://example.com/path\n")
        << pingkk::text("UDP 只有收到目标响应时才能确认连通；无响应不等于端口不通。\n");
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
    std::cout << pingkk::text("[高级信息]\n")
              << pingkk::text("  原始输入：") << target.input << "\n"
              << pingkk::text("  解析主机：") << target.host << "\n"
              << pingkk::text("  目标 IP：") << target.ip << "\n"
              << pingkk::text("  地址类型：") << (target.ip.find(':') == std::string::npos ? "IPv4" : "IPv6") << "\n";
    if (includeLocalAddress) {
        std::cout << pingkk::text("  本机出口 IP：") << pingkk::localAddressFor(target) << "\n";
    }
    std::cout << pingkk::text("  测试模式：") << mode << "\n"
              << pingkk::text("  超时时间：") << timeoutMilliseconds << pingkk::text(" 毫秒\n");
}

void printTimingStatistics(const std::string& title,
                           int sent,
                           int received,
                           const std::vector<long>& elapsedTimes) {
    const int lost = sent - received;
    const double lossRate = sent == 0 ? 0.0 : 100.0 * lost / sent;
    std::cout << pingkk::text("[高级统计] ") << title << "\n"
              << pingkk::text("  已发送：") << sent << pingkk::text("，已响应：") << received
              << pingkk::text("，丢失：") << lost << pingkk::text("（") << std::fixed << std::setprecision(1)
              << lossRate << pingkk::text("%）\n") << std::defaultfloat;
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
              << pingkk::text("  往返时延：最小 ") << minimum << pingkk::text(" 毫秒，最大 ") << maximum
              << pingkk::text(" 毫秒，平均 ") << average << pingkk::text(" 毫秒，波动 ") << variation << pingkk::text(" 毫秒\n");
    std::cout << std::defaultfloat;
}

void printProbeStatistics(int attempts,
                          int reachable,
                          int failed,
                          int unknown,
                          const std::vector<long>& successfulTimes) {
    std::cout << pingkk::text("[高级统计] 端口测试汇总\n")
              << pingkk::text("  总次数：") << attempts << pingkk::text("，连通：") << reachable
              << pingkk::text("，不通：") << failed << pingkk::text("，状态未知：") << unknown << "\n";
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
              << pingkk::text("  连通耗时：最小 ") << minimum << pingkk::text(" 毫秒，最大 ") << maximum
              << pingkk::text(" 毫秒，平均 ") << average << pingkk::text(" 毫秒，波动 ") << variation << pingkk::text(" 毫秒\n")
              << std::defaultfloat;
}

int printDnsComparison(const std::string& input,
                       int timeoutMilliseconds,
                       bool heading,
                       bool advanced,
                       pingkk::IpVersion version) {
    const std::string host = pingkk::extractHost(input);
    if (host.empty()) {
        std::cerr << pingkk::text("域名格式不正确。\n");
        return 2;
    }
    if (heading) std::cout << pingkk::text("正在对比 ") << host << pingkk::text(" 的 DNS 解析结果：\n");
    if (advanced) {
        std::cout << pingkk::text("[高级信息]\n")
                  << pingkk::text("  查询域名：") << host << "\n"
                  << (version == pingkk::IpVersion::V6 ? "  AAAA\n" : "  A\n")
                  << pingkk::text("  单次超时：") << timeoutMilliseconds << pingkk::text(" 毫秒\n")
                  << pingkk::text("  对比来源：系统 DNS、当前 DNS、阿里 DNS、腾讯 DNS\n");
    }
    const std::vector<pingkk::DnsLookupResult> results =
        pingkk::compareDns(host, timeoutMilliseconds, version);
    if (results.empty()) {
        std::cerr << pingkk::text("没有可用的 DNS 服务器。\n");
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
        std::cout << (result.success ? pingkk::text("[正常] ") : pingkk::text("[失败] "))
                  << result.resolverName << pingkk::text("（") << result.resolverAddress << pingkk::text("）");
        if (!result.success) {
            ++failedResolvers;
            if (result.resolverName == pingkk::text("阿里 DNS") || result.resolverName == pingkk::text("腾讯 DNS")) {
                ++publicResolvers;
                if (result.nameDoesNotExist) ++publicNameDoesNotExist;
                if (result.noAddressRecords) ++publicNoAddressRecords;
            }
            std::cout << pingkk::text("：") << result.error << pingkk::text("，") << result.elapsedMilliseconds << pingkk::text("毫秒\n");
            continue;
        }
        if (result.resolverName == pingkk::text("阿里 DNS") || result.resolverName == pingkk::text("腾讯 DNS")) {
            ++publicResolvers;
        } else {
            localResolverSucceeded = true;
        }
        std::cout << pingkk::text("：") << result.elapsedMilliseconds << pingkk::text("毫秒\n");
        if (version == pingkk::IpVersion::V4) {
            std::cout << pingkk::text("  IPv4：") << joinAddresses(result.ipv4Addresses) << "\n";
        } else {
            std::cout << pingkk::text("  IPv6：") << joinAddresses(result.ipv6Addresses) << "\n";
        }
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
        std::cout << pingkk::text("[高级统计] DNS 查询汇总\n")
                  << pingkk::text("  解析器：") << results.size() << pingkk::text("，成功：") << successfulResolvers
                  << pingkk::text("，失败：") << failedResolvers
                  << pingkk::text("，公共 DNS 返回 NXDOMAIN：") << publicNameDoesNotExist
                  << pingkk::text("，无 A/AAAA 记录：") << publicNoAddressRecords << "\n"
                  << std::fixed << std::setprecision(1)
                  << pingkk::text("  查询耗时：最小 ") << minimum << pingkk::text(" 毫秒，最大 ") << maximum
                  << pingkk::text(" 毫秒，平均 ") << totalElapsed / dnsElapsedTimes.size() << pingkk::text(" 毫秒\n")
                  << std::defaultfloat;
    }
    if (localResolverSucceeded && publicResolvers > 0 &&
        publicNameDoesNotExist == publicResolvers) {
        std::cout << pingkk::text("结论：DNS 解析结果存在差异。系统或当前 DNS 可以解析，")
                  << pingkk::text("但公共 DNS 返回域名不存在（NXDOMAIN）。可能原因包括内网或分区 DNS、")
                  << pingkk::text("DNS 拦截/过滤，或公网记录尚未同步；仅凭本次检测无法断定该域名只能在当前网络使用。\n");
    } else if (publicResolvers > 0 &&
               publicNoAddressRecords == publicResolvers) {
        std::cout << pingkk::text("结论：公共 DNS 均未返回所选地址类型的记录。域名可能存在，")
                  << pingkk::text("但当前没有配置可用于访问的所选类型地址。\n");
    } else if (successfulResolvers == 0) {
        std::cout << pingkk::text("结论：所有 DNS 查询均失败，请检查网络、防火墙或 DNS 设置。\n");
    } else if (failedResolvers > 0) {
        std::cout << pingkk::text("结论：部分 DNS 查询失败，不能判定结果一致；")
                  << successfulResolvers << pingkk::text(" 个解析器成功，")
                  << failedResolvers << pingkk::text(" 个失败。\n");
    } else if (distinctResults.size() > 1) {
        std::cout << pingkk::text("结论：不同 DNS 返回的 IP 不完全一致；CDN 调度可能导致差异，如访问异常请优先检查系统 DNS。\n");
    } else {
        std::cout << pingkk::text("结论：各 DNS 返回结果一致。\n");
    }
    return results[0].success ? 0 : 1;
}

int runCheckup(int timeoutMilliseconds, bool advanced, pingkk::IpVersion version) {
    const std::chrono::steady_clock::time_point checkupStart =
        std::chrono::steady_clock::now();
    std::cout << pingkk::text("开始一键网络检查……\n\n");
    if (advanced) {
        std::cout << pingkk::text("[高级信息]\n")
                  << pingkk::text("  基准域名：baidu.com\n")
                  << pingkk::text("  HTTPS 目标端口：443\n")
                  << pingkk::text("  单项超时：") << timeoutMilliseconds << pingkk::text(" 毫秒\n\n");
    }
    bool healthy = true;

    const std::string localAddress = pingkk::primaryLocalAddress(version);
    std::cout << pingkk::text("[1/5] 本机网络\n")
              << pingkk::text("  IP：") << localAddress << "\n"
              << (version == pingkk::IpVersion::V6 ? pingkk::text("  前缀：") : pingkk::text("  掩码："))
              << pingkk::networkPrefixFor(localAddress) << "\n"
              << pingkk::text("  网关：") << pingkk::defaultGateway(version) << "\n";
    if (localAddress == pingkk::text("未知")) healthy = false;

    std::cout << pingkk::text("[2/5] DNS 配置\n");
    const std::vector<std::string> dnsServers = pingkk::currentDnsServers();
    std::cout << pingkk::text("  DNS：") << joinAddresses(dnsServers) << "\n";
    if (dnsServers.empty()) healthy = false;

    std::cout << pingkk::text("[3/5] 代理设置\n");
    const std::vector<std::string> proxies = pingkk::currentProxySettings();
    if (proxies.empty()) {
        std::cout << pingkk::text("  未检测到已启用的代理。\n");
    } else {
        for (std::size_t index = 0; index < proxies.size(); ++index) {
            std::cout << "  " << proxies[index] << "\n";
        }
    }

    std::cout << pingkk::text("[4/5] 域名解析（baidu.com）\n");
    pingkk::ResolvedTarget checkTarget;
    std::string resolveError;
    const std::chrono::steady_clock::time_point resolveStart =
        std::chrono::steady_clock::now();
    const bool resolved = pingkk::resolveTarget(
        "baidu.com", checkTarget, resolveError, version);
    const long resolveElapsed = static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - resolveStart).count());
    if (!resolved) {
        std::cout << pingkk::text("  [失败] 系统 DNS 无法解析 baidu.com：")
                  << resolveError << "\n";
        healthy = false;
    } else {
        std::cout << pingkk::text("  [正常] 系统 DNS：") << resolveElapsed
                  << pingkk::text("毫秒，IP ") << checkTarget.ip << "\n";
    }

    std::cout << pingkk::text("[5/5] HTTPS 直连（baidu.com）\n");
    if (resolved) {
        const pingkk::ProbeResult probe = pingkk::probeTcp(
            checkTarget, 443, timeoutMilliseconds);
        std::cout << "  " << (probe.reachable ? pingkk::text("[正常] ") : pingkk::text("[失败] "))
                  << (version == pingkk::IpVersion::V6 ? "[" + checkTarget.ip + "]" : checkTarget.ip) << ":443 " << probe.message
                  << pingkk::text("，") << probe.elapsedMilliseconds << pingkk::text("毫秒\n");
        if (!probe.reachable) healthy = false;
    } else {
        std::cout << pingkk::text("  [跳过] 没有所选地址类型的解析结果。\n");
        healthy = false;
    }

    if (advanced) {
        const long totalElapsed = static_cast<long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - checkupStart).count());
        std::cout << pingkk::text("[高级统计] 检查汇总\n")
                  << pingkk::text("  DNS 服务器：") << dnsServers.size()
                  << pingkk::text("，已启用代理：") << proxies.size() << "\n"
                  << pingkk::text("  DNS 解析耗时：") << resolveElapsed << pingkk::text(" 毫秒\n")
                  << pingkk::text("  总耗时：") << totalElapsed << pingkk::text(" 毫秒\n");
    }

    std::cout << pingkk::text("\n检查结论：")
              << (healthy ? pingkk::text("基础网络状态正常。") : pingkk::text("发现异常，请根据上方失败项目继续排查。"))
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

const char* versionName(pingkk::IpVersion version) {
    return version == pingkk::IpVersion::V6 ? "IPv6" : "IPv4";
}

std::vector<pingkk::IpVersion> selectedVersions(pingkk::IpVersion version) {
    if (version == pingkk::IpVersion::Both) {
        return {pingkk::IpVersion::V4, pingkk::IpVersion::V6};
    }
    return {version};
}

std::vector<pingkk::ResolvedTarget> resolveSelectedTargets(
        const std::string& input, pingkk::IpVersion version, bool& complete) {
    std::vector<pingkk::ResolvedTarget> targets;
    complete = true;
    const std::vector<pingkk::IpVersion> versions = selectedVersions(version);
    for (std::size_t index = 0; index < versions.size(); ++index) {
        pingkk::ResolvedTarget target;
        std::string error;
        if (pingkk::resolveTarget(input, target, error, versions[index])) {
            targets.push_back(target);
        } else {
            complete = false;
            std::cerr << "[" << versionName(versions[index]) << "] "
                      << pingkk::text("无法解析目标地址：") << error << "\n";
        }
    }
    return targets;
}

int runPingOrRoute(const std::string& input,
                   bool route,
                   bool continuous,
                   int timeoutMilliseconds,
                   bool advanced,
                   pingkk::IpVersion version) {
    const std::string host = pingkk::extractHost(input);
    if (host.empty()) {
        std::cerr << pingkk::text("目标地址格式不正确。\n");
        return 2;
    }

    bool complete = true;
    const std::vector<pingkk::ResolvedTarget> targets = resolveSelectedTargets(host, version, complete);
    if (targets.empty()) return 2;
    for (std::size_t index = 0; index < targets.size(); ++index) {
        const pingkk::ResolvedTarget& target = targets[index];
        if (advanced) {
            printAdvancedTarget(target, route ? pingkk::text("路由追踪") :
                (continuous ? pingkk::text("持续 Ping") : "Ping"), timeoutMilliseconds, true);
        }
        if (target.host != target.ip) {
            std::cout << pingkk::text("域名 ") << target.host << pingkk::text(" 解析为 IP ") << target.ip << "\n";
        }
    }
    printCurrentDns();

    const auto trace = [&](const pingkk::ResolvedTarget& target) -> int {
        const std::chrono::steady_clock::time_point routeStart =
            std::chrono::steady_clock::now();
        int attemptedHops = 0;
        int respondingHops = 0;
        const auto printRouteSummary = [&]() {
            if (!advanced) return;
            const long totalElapsed = static_cast<long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - routeStart).count());
            std::cout << pingkk::text("[高级统计] 路由追踪汇总\n")
                      << pingkk::text("  已探测跳数：") << attemptedHops
                      << pingkk::text("，有响应：") << respondingHops
                      << pingkk::text("，无响应：") << attemptedHops - respondingHops << "\n"
                      << pingkk::text("  总耗时：") << totalElapsed << pingkk::text(" 毫秒\n");
        };
        std::cout << pingkk::text("正在追踪到 ") << target.ip << pingkk::text(" 的路由，最多 30 跳：\n");
        for (int hop = 1; hop <= 30 && g_running; ++hop) {
            const pingkk::TraceHop result = pingkk::traceHop(
                target, hop, timeoutMilliseconds, static_cast<unsigned short>(hop));
            ++attemptedHops;
            if (!g_running) break;
            std::cout << ' ' << hop << "  ";
            if (!result.responded) {
                std::cout << "*";
                if (!result.error.empty() && result.error != pingkk::text("请求超时")) {
                    std::cout << "  " << result.error << "\n";
                    printRouteSummary();
                    return 1;
                }
            } else {
                ++respondingHops;
                std::cout << result.address << "  " << result.elapsedMilliseconds << pingkk::text("毫秒");
                if (advanced) std::cout << "  TTL=" << hop;
            }
            std::cout << "\n";
            if (result.destinationReached) {
                std::cout << pingkk::text("路由追踪完成。\n");
                printRouteSummary();
                return 0;
            }
        }
        if (!g_running) {
            printRouteSummary();
            return 130;
        }
        std::cout << pingkk::text("已达到最大跳数，目标尚未响应。\n");
        printRouteSummary();
        return 1;
    };
    if (route) {
        int exitCode = complete ? 0 : 1;
        for (std::size_t index = 0; index < targets.size() && g_running; ++index) {
            const int result = trace(targets[index]);
            if (result != 0) exitCode = result;
        }
        return g_running ? exitCode : 130;
    }

    struct PingStatistics {
        bool anyReply = false;
        bool fatal = false;
        int sent = 0;
        int received = 0;
        std::vector<long> times;
    };
    std::vector<PingStatistics> statistics(targets.size());
    for (std::size_t index = 0; index < targets.size(); ++index) {
        std::cout << pingkk::text("正在 Ping ") << targets[index].ip << pingkk::text("：\n");
    }
    unsigned short sequence = 1;
    unsigned int rounds = 0;
    do {
        const std::chrono::steady_clock::time_point iterationStart = std::chrono::steady_clock::now();
        bool active = false;
        for (std::size_t index = 0; index < targets.size() && g_running; ++index) {
            PingStatistics& stats = statistics[index];
            if (stats.fatal) continue;
            active = true;
            const pingkk::ResolvedTarget& target = targets[index];
            ++stats.sent;
        if (targets.size() > 1) std::cout << "[" << (target.ip.find(':') == std::string::npos ? "IPv4" : "IPv6") << "] ";
            const pingkk::PingResult result = pingkk::ping(
                target, timeoutMilliseconds, sequence);
            if (result.reachable) {
                stats.anyReply = true;
                ++stats.received;
                stats.times.push_back(result.elapsedMilliseconds);
                std::cout << pingkk::text("来自 ") << result.replyAddress << pingkk::text("：时间=")
                          << result.elapsedMilliseconds << pingkk::text("毫秒");
                if (result.ttl >= 0) std::cout << " TTL=" << result.ttl;
                if (advanced) {
                    std::cout << pingkk::text(" 序号=") << sequence
                              << pingkk::text(" 数据=") << result.payloadBytes << pingkk::text("字节")
                              << pingkk::text(" IP包=") << result.packetBytes << pingkk::text("字节");
                }
                std::cout << "\n";
            } else {
                if (advanced) {
                    std::cout << pingkk::text("序号=") << sequence << " " << result.error
                              << pingkk::text(" 数据=") << result.payloadBytes << pingkk::text("字节")
                              << pingkk::text(" IP包=") << result.packetBytes << pingkk::text("字节\n");
                } else {
                    std::cout << result.error << "\n";
                }
                if (!result.error.empty() && result.error != pingkk::text("请求超时") &&
                    result.error != pingkk::text("探测已中断")) {
                    stats.fatal = true;
                }
            }
        }
        ++rounds;
        if (!g_running || !active) break;
        ++sequence;
        if (continuous || rounds < 4) waitUntilOrStopped(iterationStart + std::chrono::seconds(1));
    } while (g_running && (continuous || rounds < 4));
    bool passed = complete;
    for (std::size_t index = 0; index < statistics.size(); ++index) {
        const PingStatistics& stats = statistics[index];
        if (advanced) {
            const bool ipv6 = targets[index].ip.find(':') != std::string::npos;
            printTimingStatistics(std::string(pingkk::text("Ping 汇总")) + " " + targets[index].ip,
                                  stats.sent, stats.received, stats.times);
            std::cout << pingkk::text("  ICMP 数据：") << 16
                      << (ipv6 ? pingkk::text(" 字节，IPv6 包：") : pingkk::text(" 字节，IPv4 包："))
                      << (ipv6 ? 64 : 44) << pingkk::text(" 字节（不含链路层开销）\n");
        }
        if (!stats.anyReply || stats.fatal) passed = false;
    }
    if (!g_running) return 130;
    return passed ? 0 : 1;
}

void printProbe(const pingkk::ResolvedTarget& target,
                unsigned short port,
                const pingkk::ProbeResult& result) {
    std::cout << pingkk::text("对 ") << target.ip << pingkk::text(" 的 ") << port << pingkk::text(" 端口测试")
              << "........" << pingkk::protocolName(result.protocol) << pingkk::text(" 协议");
    if (result.reachable) {
        std::cout << pingkk::text("连通");
    } else if (!result.definitive) {
        std::cout << pingkk::text("状态未知");
    } else {
        std::cout << pingkk::text("不通");
    }
    std::cout << pingkk::text("（") << result.message << pingkk::text("，") << result.elapsedMilliseconds << pingkk::text("毫秒）\n");
}

int showGuiDownloadHint() {
    std::cerr << pingkk::text("未找到图形界面，请下载适合当前系统的图形界面包或安装包：\n")
              << "1. GitHub Releases: " PINGKK_PROJECT_URL "/releases\n"
              << pingkk::text("2. 网站下载：https://he.sb/pingkk/\n");
    return 1;
}

int showGuiLaunchError() {
    std::cerr << pingkk::text("图形界面启动失败，请检查安装文件和运行环境。\n");
    return 1;
}

int launchGui(const char* executablePath) {
#ifdef __APPLE__
    (void)executablePath;
    CFErrorRef error = NULL;
    CFArrayRef applications = LSCopyApplicationURLsForBundleIdentifier(CFSTR("cn.trah.pingkk"), &error);
    char path[32768] = {0};
    bool found = false;
    if (applications != NULL) {
        for (CFIndex index = 0; index < CFArrayGetCount(applications); ++index) {
            CFURLRef application = static_cast<CFURLRef>(CFArrayGetValueAtIndex(applications, index));
            if (CFURLGetFileSystemRepresentation(application, true,
                    reinterpret_cast<UInt8*>(path), sizeof(path)) && access(path, F_OK) == 0) {
                found = true;
                break;
            }
        }
        CFRelease(applications);
    }
    if (error != NULL) CFRelease(error);
    if (!found) return showGuiDownloadHint();
    return runSystemTool(std::vector<std::string>{"open", "-a", path}) == 0
               ? 0 : showGuiLaunchError();
#elif defined(_WIN32)
    (void)executablePath;
    std::vector<wchar_t> executable(32768, 0);
    const DWORD length = GetModuleFileNameW(NULL, &executable[0], static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) return showGuiLaunchError();
    std::wstring path(&executable[0], length);
    const std::wstring::size_type slash = path.find_last_of(L"/\\");
    path = path.substr(0, slash + 1) + L"pingkk-gui.exe";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                   ? showGuiDownloadHint() : showGuiLaunchError();
    }
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(path.c_str(), NULL, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        return showGuiLaunchError();
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
#else
    std::string path(executablePath);
    char executable[4096];
    const ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable));
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(executable)) return showGuiLaunchError();
    path.assign(executable, static_cast<std::size_t>(length));
    const std::string::size_type slash = path.find_last_of('/');
    path = (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) + "pingkk-gui";
    if (access(path.c_str(), F_OK) != 0) {
        return errno == ENOENT ? showGuiDownloadHint() : showGuiLaunchError();
    }
    return runSystemTool(std::vector<std::string>{path}) == 0 ? 0 : showGuiLaunchError();
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
    for (std::size_t index = 0; index < arguments.size();) {
        if (arguments[index] != "--lang") {
            ++index;
            continue;
        }
        if (index + 1 >= arguments.size() ||
            (arguments[index + 1] != "zh" && arguments[index + 1] != "en")) {
            std::cerr << "--lang: zh | en\n";
            return 2;
        }
        pingkk::setEnglish(arguments[index + 1] == "en");
        arguments.erase(arguments.begin() + index, arguments.begin() + index + 2);
    }
    if (arguments.empty()) {
        printHelp();
        return 0;
    }
    if (std::find(arguments.begin(), arguments.end(), "-h") != arguments.end() ||
        std::find(arguments.begin(), arguments.end(), "--help") != arguments.end()) {
        printHelp();
        return 0;
    }
    if (arguments[0] == "gui") {
        return launchGui(argv[0]);
    }

    int timeoutMilliseconds = 1000;
    pingkk::IpVersion version = pingkk::IpVersion::V4;
    bool versionSpecified = false;
    bool continuous = false;
    bool route = false;
    bool checkup = false;
    bool dnsComparison = false;
    bool advanced = false;
    std::vector<std::string> positional;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string& argument = arguments[index];
        if (argument == "--timeout" || argument == "-w") {
            if (index + 1 >= arguments.size() ||
                !parseTimeout(arguments[++index], timeoutMilliseconds)) {
                std::cerr << pingkk::text("超时时间必须是 100 到 60000 之间的毫秒数。\n");
                return 2;
            }
        } else if (argument == "-v6" || argument == "-v46") {
            const pingkk::IpVersion selected = argument == "-v6" ? pingkk::IpVersion::V6 : pingkk::IpVersion::Both;
            if (versionSpecified && selected != version) {
                std::cerr << pingkk::text("-v6 和 -v46 不能同时使用。\n");
                return 2;
            }
            version = selected;
            versionSpecified = true;
        } else if (argument == "-t") {
            continuous = true;
        } else if (argument == "-r" || argument == "--route") {
            route = true;
        } else if (argument == "-jc" || argument == "--checkup") {
            checkup = true;
        } else if (argument == "--dns") {
            dnsComparison = true;
        } else if (argument == "-a" || argument == "--advanced") {
            advanced = true;
        } else if (!argument.empty() && argument[0] == '-') {
            std::cerr << pingkk::text("未知选项：") << argument << "\n";
            return 2;
        } else {
            positional.push_back(argument);
        }
    }
    if (static_cast<int>(route) + static_cast<int>(checkup) +
        static_cast<int>(dnsComparison) > 1) {
        std::cerr << pingkk::text("路由追踪、网络检查和 DNS 对比不能同时使用。\n");
        return 2;
    }
    if (continuous && (checkup || dnsComparison)) {
        std::cerr << pingkk::text("持续测试只能用于 Ping 或端口测试。\n");
        return 2;
    }
    if (checkup) {
        if (!positional.empty()) {
            std::cerr << pingkk::text("一键网络检查不需要目标地址。\n");
            return 2;
        }
        int exitCode = 0;
        const std::vector<pingkk::IpVersion> versions = selectedVersions(version);
        for (std::size_t index = 0; index < versions.size() && g_running; ++index) {
            if (version == pingkk::IpVersion::Both) std::cout << "[" << versionName(versions[index]) << "]\n";
            const int result = runCheckup(timeoutMilliseconds, advanced, versions[index]);
            if (result != 0) exitCode = result;
        }
        return g_running ? exitCode : 130;
    }
    if (dnsComparison) {
        if (positional.size() != 1) {
            std::cerr << pingkk::text("DNS 对比需要一个域名。\n");
            return 2;
        }
        int exitCode = 0;
        const std::vector<pingkk::IpVersion> versions = selectedVersions(version);
        for (std::size_t index = 0; index < versions.size() && g_running; ++index) {
            if (version == pingkk::IpVersion::Both) std::cout << "[" << versionName(versions[index]) << "]\n";
            const int result = printDnsComparison(positional[0], timeoutMilliseconds, true, advanced, versions[index]);
            if (result != 0) exitCode = result;
        }
        return g_running ? exitCode : 130;
    }
    if (positional.empty()) {
        std::cerr << pingkk::text("缺少目标地址。\n");
        return 2;
    }
    if (route) {
        if (continuous) {
            std::cerr << pingkk::text("持续测试不能与路由追踪同时使用。\n");
            return 2;
        }
        if (positional.size() != 1) {
            std::cerr << pingkk::text("路由追踪只需要目标地址。\n");
            return 2;
        }
        return runPingOrRoute(positional[0], true, false, timeoutMilliseconds, advanced, version);
    }
    if (positional.size() == 1) {
        return runPingOrRoute(positional[0], false, continuous, timeoutMilliseconds, advanced, version);
    }

    std::string address;
    std::string protocol;
    std::vector<unsigned short> ports;
    std::string parseError;
    if (!parseTestArguments(positional, address, ports, protocol, parseError)) {
        std::cerr << parseError << "\n";
        return 2;
    }

    bool complete = true;
    const std::vector<pingkk::ResolvedTarget> targets = resolveSelectedTargets(address, version, complete);
    if (targets.empty()) return 2;
    for (std::size_t index = 0; index < targets.size(); ++index) {
        const pingkk::ResolvedTarget& target = targets[index];
        if (advanced) {
            std::string mode = protocol == "tcp" ? pingkk::text("TCP 端口测试") :
                               protocol == "udp" ? pingkk::text("UDP 端口测试") :
                                                   pingkk::text("TCP + UDP 端口测试");
            if (continuous) mode = pingkk::text("持续") + mode;
            printAdvancedTarget(target, mode, timeoutMilliseconds, false);
            std::cout << pingkk::text("  目标端口：");
            for (std::size_t index = 0; index < ports.size(); ++index) {
                if (index > 0) std::cout << ", ";
                std::cout << ports[index];
            }
            std::cout << "\n";
        }
        std::cout << pingkk::text("当前 IP：") << pingkk::localAddressFor(target) << "\n";
        if (target.host != target.ip) {
            std::cout << pingkk::text("域名 ") << target.host << pingkk::text(" 解析为 IP ") << target.ip << "\n";
        }
        printCurrentDns();

    }
    bool allReachable = complete;
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
        for (std::size_t targetIndex = 0; targetIndex < targets.size() && g_running; ++targetIndex) {
            const pingkk::ResolvedTarget& target = targets[targetIndex];
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
