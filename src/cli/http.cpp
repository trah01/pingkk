#include "http.h"
#include "pingkk/http_check.h"
#include "pingkk/language.h"

#include <iomanip>
#include <iostream>
#include <sstream>

namespace pingkk { namespace cli {
namespace {

std::string milliseconds(double value) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(2) << value << " ms";
    return output.str();
}

// 服务端返回的网址不能向终端输出控制字符。
std::string printable(const std::string& value) {
    std::string output;
    for (unsigned char ch : value) {
        if (ch >= 0x20 && ch != 0x7f) output.push_back(static_cast<char>(ch));
    }
    return output;
}

}  // namespace

int runHttpCheck(const std::string& url, int timeoutMilliseconds, bool advanced,
                 IpVersion version, const std::function<bool()>& keepRunning,
                 const std::string& proxy) {
    std::cout << text("HTTP 检测：") << printable(url) << "\n";
    if (!proxy.empty()) {
        std::cout << text("连接方式：代理\n")
                  << text("当前 IP：") << primaryLocalAddress(version) << "\n";
    }
    const HttpCheckResult result = checkHttp(url, timeoutMilliseconds, version, keepRunning, proxy);
    if (!result.localIp.empty()) {
        std::cout << (proxy.empty() ? text("当前 IP：") : text("代理连接本地 IP："))
                  << result.localIp << "\n";
    }
    if (!result.remoteIp.empty()) {
        std::cout << (proxy.empty() ? text("目标 IP：") : text("代理 IP：")) << result.remoteIp << "\n";
    }
    if (result.statusCode != 0) {
        std::cout << text("HTTP 状态码：") << result.statusCode << "\n";
    }
    if (result.redirects > 0) {
        std::cout << text("重定向次数：") << result.redirects << "\n"
                  << text("最终网址：") << printable(result.effectiveUrl) << "\n";
    }
    if (result.tlsVerified) std::cout << text("HTTPS 证书：校验通过\n");
    std::cout << text("DNS 解析耗时：") << milliseconds(result.dnsMilliseconds) << "\n"
              << text("建立连接耗时：") << milliseconds(result.connectMilliseconds) << "\n";
    if (result.tlsMilliseconds > 0 || result.tlsVerified) {
        std::cout << text("TLS 握手耗时：") << milliseconds(result.tlsMilliseconds) << "\n";
    }
    if (advanced && result.firstByteMilliseconds > 0) {
        std::cout << text("首字节耗时：")
                  << milliseconds(result.firstByteMilliseconds) << "\n";
    }
    std::cout << text("总耗时：") << milliseconds(result.totalMilliseconds) << "\n";
    if (!result.error.empty()) {
        std::cout << text("[失败] ") << result.error << "\n";
    } else if (result.success) {
        std::cout << text("[正常] HTTP 请求完成。\n");
    } else {
        std::cout << text("[失败] 网站已响应，但返回错误状态码。\n");
    }
    return result.success ? 0 : 1;
}

} }
