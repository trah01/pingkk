#include "pingkk/core.h"
#include "pingkk/diagnostics.h"
#include "pingkk/version.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <sstream>
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
        << "  --timeout <毫秒>                        设置超时时间，默认 1000\n"
        << "  pingkk gui                            打开图形界面\n"
        << "  pingkk -h | --help                    显示本教程\n\n"
        << "选项 -t、-r、--route、--timeout 和 -w 可以放在命令中的任意位置。\n"
        << "一次只能测试一个地址；多个端口使用英文逗号分隔。\n"
        << "地址可以填写 IP、域名或完整网址，例如：https://example.com/path\n"
        << "UDP 只有收到目标响应时才能确认连通；无响应不等于端口不通。\n";
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
                   int timeoutMilliseconds) {
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
    if (target.host != target.ip) {
        std::cout << "域名 " << target.host << " 解析为 IP " << target.ip << "\n";
    }
    printCurrentDns();

    if (route) {
        std::cout << "正在追踪到 " << target.ip << " 的路由，最多 30 跳：\n";
        for (int hop = 1; hop <= 30 && g_running; ++hop) {
            const pingkk::TraceHop result = pingkk::traceHop(
                target, hop, timeoutMilliseconds, static_cast<unsigned short>(hop));
            if (!g_running) return 130;
            std::cout << ' ' << hop << "  ";
            if (!result.responded) {
                std::cout << "*";
                if (!result.error.empty() && result.error != "请求超时") {
                    std::cout << "  " << result.error << "\n";
                    return 1;
                }
            } else {
                std::cout << result.address << "  " << result.elapsedMilliseconds << "毫秒";
            }
            std::cout << "\n";
            if (result.destinationReached) {
                std::cout << "路由追踪完成。\n";
                return 0;
            }
        }
        if (!g_running) return 130;
        std::cout << "已达到最大跳数，目标尚未响应。\n";
        return 1;
    }

    bool anyReply = false;
    std::cout << "正在 Ping " << target.ip << "：\n";
    unsigned short sequence = 1;
    do {
        const std::chrono::steady_clock::time_point iterationStart =
            std::chrono::steady_clock::now();
        const pingkk::PingResult result = pingkk::ping(
            target, timeoutMilliseconds, sequence);
        if (!g_running) return 130;
        if (result.reachable) {
            anyReply = true;
            std::cout << "来自 " << result.replyAddress << "：时间="
                      << result.elapsedMilliseconds << "毫秒";
            if (result.ttl >= 0) std::cout << " TTL=" << result.ttl;
            std::cout << "\n";
        } else {
            std::cout << result.error << "\n";
            if (!result.error.empty() && result.error != "请求超时") return 1;
        }
        ++sequence;
        const bool hasNextProbe = g_running && (continuous || sequence <= 4);
        if (hasNextProbe) {
            const long elapsed = static_cast<long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - iterationStart)
                    .count());
            if (elapsed < 1000) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000 - elapsed));
            }
        }
    } while (g_running && (continuous || sequence <= 4));
    if (!g_running) return 130;
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
    for (std::vector<std::string>::iterator it = arguments.begin(); it != arguments.end();) {
        if (*it == "-t") {
            continuous = true;
            it = arguments.erase(it);
        } else if (*it == "-r" || *it == "--route") {
            route = true;
            it = arguments.erase(it);
        } else {
            ++it;
        }
    }
    if (arguments.empty()) {
        std::cerr << "缺少目标地址。\n";
        return 2;
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
        return runPingOrRoute(arguments[0], true, false, timeoutMilliseconds);
    }

    if (arguments.size() == 1) {
        return runPingOrRoute(arguments[0], false, continuous, timeoutMilliseconds);
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
    std::cout << "当前 IP：" << pingkk::localAddressFor(target) << "\n";
    if (target.host != target.ip) {
        std::cout << "域名 " << target.host << " 解析为 IP " << target.ip << "\n";
    }
    printCurrentDns();

    bool allReachable = true;
    do {
        for (std::size_t portIndex = 0;
             portIndex < ports.size() && g_running;
             ++portIndex) {
            if (protocol == "tcp" || protocol == "all") {
                const pingkk::ProbeResult result = pingkk::probeTcp(
                    target, ports[portIndex], timeoutMilliseconds);
                printProbe(target, ports[portIndex], result);
                if (!result.reachable) allReachable = false;
            }
            if ((protocol == "udp" || protocol == "all") && g_running) {
                const pingkk::ProbeResult result = pingkk::probeUdp(
                    target, ports[portIndex], timeoutMilliseconds);
                printProbe(target, ports[portIndex], result);
                if (!result.reachable) allReachable = false;
            }
        }
        if (continuous && g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    } while (continuous && g_running);

    if (!g_running) return 130;
    return allReachable ? 0 : 1;
}
