#include "arguments.h"
#include "pingkk/core.h"
#include "pingkk/language.h"
#include <cstdlib>

namespace pingkk { namespace cli {
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

bool isPortArgument(const std::string& value) {
    return !value.empty() && value.find_first_not_of("0123456789, \t") == std::string::npos;
}

bool isProtocolArgument(const std::string& value) {
    return value == "tcp" || value == "udp" || value == "all";
}

bool parseTestArguments(const std::vector<std::string>& arguments,
                        std::string& address,
                        std::vector<unsigned short>& ports,
                        std::string& protocol,
                        std::string& error) {
    // Prefer a domain, IP, or URL over numeric ports and protocol names.
    // With only numeric arguments, preserve the first argument as the target.
    std::size_t targetIndex = 0;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (!isPortArgument(arguments[index]) && !isProtocolArgument(arguments[index])) {
            targetIndex = index;
            break;
        }
    }
    address = arguments[targetIndex];
    if (pingkk::extractHost(address).find(',') != std::string::npos) {
        error = pingkk::text("一次只能测试一个目标地址。");
        return false;
    }
    bool hasPorts = false;
    bool hasProtocol = false;
    protocol = "tcp";
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (index == targetIndex) continue;
        const std::string& argument = arguments[index];
        if (isProtocolArgument(argument)) {
            if (hasProtocol) {
                error = pingkk::text("只能指定一个协议：tcp、udp 或 all。");
                return false;
            }
            protocol = argument;
            hasProtocol = true;
        } else if (isPortArgument(argument)) {
            if (hasPorts || !parsePorts(argument, ports)) {
                error = pingkk::text("请指定一组 1 到 65535 之间的端口，多个端口用逗号分隔。");
                return false;
            }
            hasPorts = true;
        } else {
            error = std::string(pingkk::text("无法识别参数：")) + argument +
                    pingkk::text("。请只填写一个目标地址、数字端口和 tcp、udp 或 all 协议。");
            return false;
        }
    }
    if (!hasPorts) {
        error = pingkk::text("指定协议时需要同时填写端口。");
        return false;
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


} }
