#ifndef PINGKK_HTTP_CHECK_H
#define PINGKK_HTTP_CHECK_H

#include "pingkk/core.h"
#include <functional>
#include <string>

namespace pingkk {

struct HttpCheckResult {
    bool success = false;
    bool tlsVerified = false;
    long statusCode = 0;
    long redirects = 0;
    double dnsMilliseconds = 0;
    double connectMilliseconds = 0;
    double tlsMilliseconds = 0;
    double firstByteMilliseconds = 0;
    double totalMilliseconds = 0;
    std::string effectiveUrl;
    std::string remoteIp;
    std::string localIp;
    std::string error;
};

bool isHttpUrl(const std::string& url);

// 发送 GET 请求，丢弃正文；默认直连，可指定代理；验证证书并最多跟随 10 次 HTTP(S) 重定向。
HttpCheckResult checkHttp(const std::string& url, int timeoutMilliseconds,
                          IpVersion version = IpVersion::V4,
                          const std::function<bool()>& keepRunning = std::function<bool()>(),
                          const std::string& proxy = std::string());

}  // namespace pingkk

#endif
