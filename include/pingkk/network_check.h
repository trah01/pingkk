#ifndef PINGKK_NETWORK_CHECK_H
#define PINGKK_NETWORK_CHECK_H

#include <string>
#include <vector>

namespace pingkk {

struct DnsLookupResult {
    std::string resolverName;
    std::string resolverAddress;
    bool success;
    bool nameDoesNotExist;
    bool noAddressRecords;
    long elapsedMilliseconds;
    std::vector<std::string> ipv4Addresses;
    std::vector<std::string> ipv6Addresses;
    std::string error;
};

// 使用系统解析器、当前 DNS 及常用公共 DNS 对比查询 A/AAAA 记录。
std::vector<DnsLookupResult> compareDns(const std::string& host,
                                        int timeoutMilliseconds);

// 获取默认网关；无法确定时返回“未知”。
std::string defaultGateway();

// 获取已启用的代理设置，结果不包含用户名或密码。
std::vector<std::string> currentProxySettings();

}  // namespace pingkk

#endif
