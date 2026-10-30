#include "pingkk/http_check.h"
#include "pingkk/language.h"
#include "pingkk/version.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <curl/curl.h>
#include <algorithm>
#include <chrono>
#include <cctype>

namespace pingkk {
namespace {

class CurlRuntime {
public:
    CurlRuntime() : status(curl_global_init(CURL_GLOBAL_DEFAULT)) {}
    ~CurlRuntime() { if (status == CURLE_OK) curl_global_cleanup(); }
    CURLcode status;
};

std::string schemeOf(const std::string& url) {
    const std::string::size_type end = url.find("://");
    if (end == std::string::npos) return std::string();
    std::string scheme = url.substr(0, end);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return scheme;
}

std::size_t discardBody(char*, std::size_t size, std::size_t count, void*) {
    return size * count;
}

int checkCancelled(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const std::function<bool()>* keepRunning =
        static_cast<const std::function<bool()>*>(context);
    return *keepRunning && !(*keepRunning)() ? 1 : 0;
}

template<typename T>
void setOption(CURL* curl, CURLoption option, T value, CURLcode& status) {
    if (status == CURLE_OK) status = curl_easy_setopt(curl, option, value);
}

std::string httpError(CURLcode status) {
    switch (status) {
    case CURLE_UNSUPPORTED_PROTOCOL:
    case CURLE_URL_MALFORMAT: return text("请输入有效的 HTTP 或 HTTPS 网址。");
    case CURLE_COULDNT_RESOLVE_HOST: return text("域名解析失败，请检查地址和 DNS 设置。");
    case CURLE_COULDNT_RESOLVE_PROXY: return text("代理地址解析失败，请检查代理设置。");
    case CURLE_COULDNT_CONNECT: return text("无法连接网站，请检查网络和目标端口。");
    case CURLE_OPERATION_TIMEDOUT: return text("HTTP 请求超时，可增加超时时间后重试。");
    case CURLE_PEER_FAILED_VERIFICATION: return text("HTTPS 证书校验失败，请检查证书和系统时间。");
    case CURLE_SSL_CONNECT_ERROR: return text("TLS 握手失败，请检查网站的 HTTPS 配置。");
    case CURLE_SSL_CACERT_BADFILE: return text("无法读取可信证书，请检查系统证书配置。");
    case CURLE_ABORTED_BY_CALLBACK: return text("探测已中断");
    default: return curl_easy_strerror(status);
    }
}

double timeInfo(CURL* curl, CURLINFO info) {
    double seconds = 0;
    curl_easy_getinfo(curl, info, &seconds);
    return seconds * 1000;
}

std::string stringInfo(CURL* curl, CURLINFO info) {
    char* value = NULL;
    curl_easy_getinfo(curl, info, &value);
    return value == NULL ? std::string() : std::string(value);
}

CURLcode configureRequest(CURL* curl, IpVersion version,
                          const std::function<bool()>& keepRunning, const std::string& proxy) {
    // 重置请求选项，避免把上一个站点的认证信息用于跳转后的站点。
    curl_easy_reset(curl);
    CURLcode status = CURLE_OK;
    setOption(curl, CURLOPT_NOSIGNAL, 1L, status);
    setOption(curl, CURLOPT_WRITEFUNCTION, &discardBody, status);
    setOption(curl, CURLOPT_XFERINFOFUNCTION, &checkCancelled, status);
    setOption(curl, CURLOPT_XFERINFODATA, &keepRunning, status);
    setOption(curl, CURLOPT_NOPROGRESS, 0L, status);
    setOption(curl, CURLOPT_USERAGENT, "pingkk/" PINGKK_VERSION, status);
    setOption(curl, CURLOPT_IPRESOLVE,
              static_cast<long>(version == IpVersion::V6 ? CURL_IPRESOLVE_V6 : CURL_IPRESOLVE_V4), status);
    // 显式选择直连或指定代理，不受环境变量和代理绕过列表影响。
    setOption(curl, CURLOPT_PROXY, proxy.c_str(), status);
    setOption(curl, CURLOPT_NOPROXY, "", status);
    setOption(curl, CURLOPT_SSL_VERIFYPEER, 1L, status);
    setOption(curl, CURLOPT_SSL_VERIFYHOST, 2L, status);
#if LIBCURL_VERSION_NUM >= 0x075500
    // 新 SDK 编译的 macOS 程序可能运行在提供旧 libcurl 的系统上。
    if (curl_version_info(CURLVERSION_NOW)->version_num >= 0x075500) {
        setOption(curl, CURLOPT_PROTOCOLS_STR, "http,https", status);
    } else
#endif
    {
        setOption(curl, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS), status);
    }
    return status;
}

}  // namespace

bool isHttpUrl(const std::string& url) {
    const std::string scheme = schemeOf(url);
    return scheme == "http" || scheme == "https";
}

HttpCheckResult checkHttp(const std::string& url, int timeoutMilliseconds,
                          IpVersion version, const std::function<bool()>& keepRunning,
                          const std::string& proxy) {
    HttpCheckResult result;
    if (!proxy.empty()) {
        const std::string scheme = schemeOf(proxy);
        if ((scheme != "http" && scheme != "https" && scheme != "socks5" && scheme != "socks5h") ||
            proxy.find_first_of("\r\n\t ") != std::string::npos) {
            result.error = text("代理地址需以 http://、https://、socks5:// 或 socks5h:// 开头。");
            return result;
        }
    }
    if (!isHttpUrl(url)) {
        result.error = text("请输入有效的 HTTP 或 HTTPS 网址。");
        return result;
    }
    if (timeoutMilliseconds < 100 || timeoutMilliseconds > 60000) {
        result.error = text("超时时间必须是 100 到 60000 之间的毫秒数。\n");
        return result;
    }
    static CurlRuntime runtime;
    if (runtime.status != CURLE_OK) {
        result.error = text("网络组件初始化失败");
        return result;
    }
    CURL* curl = curl_easy_init();
    if (curl == NULL) {
        result.error = text("无法创建 HTTP 检测。");
        return result;
    }

    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::string currentUrl = url;
    for (;;) {
        CURLcode status = configureRequest(curl, version, keepRunning, proxy);
        if (status != CURLE_OK) { result.error = httpError(status); break; }
        if (keepRunning && !keepRunning()) {
            result.error = text("探测已中断");
            break;
        }
        const double spentMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        const long spent = static_cast<long>(spentMilliseconds);
        if (spent >= timeoutMilliseconds) {
            result.error = httpError(CURLE_OPERATION_TIMEDOUT);
            break;
        }
        setOption(curl, CURLOPT_URL, currentUrl.c_str(), status);
        setOption(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeoutMilliseconds) - spent, status);
        // 每个跳转单独统计，确保 HTTP 与 HTTPS 混合跳转的耗时也可正确相加。
        if (status == CURLE_OK) status = curl_easy_perform(curl);
        const double dns = timeInfo(curl, CURLINFO_NAMELOOKUP_TIME);
        const double connected = timeInfo(curl, CURLINFO_CONNECT_TIME);
        const double tls = timeInfo(curl, CURLINFO_APPCONNECT_TIME);
        result.dnsMilliseconds += dns;
        result.connectMilliseconds += std::max(0.0, connected - dns);
        result.tlsMilliseconds += tls > 0 ? std::max(0.0, tls - connected) : 0;
        const double firstByte = timeInfo(curl, CURLINFO_STARTTRANSFER_TIME);
        result.firstByteMilliseconds = firstByte > 0 ? spentMilliseconds + firstByte : 0;
        result.effectiveUrl = stringInfo(curl, CURLINFO_EFFECTIVE_URL);
        result.remoteIp = stringInfo(curl, CURLINFO_PRIMARY_IP);
        result.localIp = stringInfo(curl, CURLINFO_LOCAL_IP);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.statusCode);
        long proxyStatus = 0;
        curl_easy_getinfo(curl, CURLINFO_HTTP_CONNECTCODE, &proxyStatus);
        if (!proxy.empty() && (result.statusCode == 407 || proxyStatus == 407)) {
            result.error = text("代理要求身份验证，请检查代理设置。");
            break;
        }
        result.tlsVerified = status == CURLE_OK && schemeOf(currentUrl) == "https";
        if (status != CURLE_OK) { result.error = httpError(status); break; }

        const std::string redirect = stringInfo(curl, CURLINFO_REDIRECT_URL);
        if (redirect.empty()) {
            result.success = result.statusCode >= 200 && result.statusCode < 400;
            break;
        }
        if (!isHttpUrl(redirect)) {
            result.error = text("重定向到了非 HTTP 或 HTTPS 地址，已停止。");
            break;
        }
        if (result.redirects >= 10) {
            result.error = text("重定向超过 10 次，请检查网站配置。");
            break;
        }
        ++result.redirects;
        currentUrl = redirect;
    }
    curl_easy_cleanup(curl);
    result.totalMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    return result;
}

}  // namespace pingkk
