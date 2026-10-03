#ifndef PINGKK_CLI_ARGUMENTS_H
#define PINGKK_CLI_ARGUMENTS_H
#include <string>
#include <vector>
namespace pingkk { namespace cli {
bool parseTestArguments(const std::vector<std::string>& arguments,
                        std::string& address, std::vector<unsigned short>& ports,
                        std::string& protocol, std::string& error);
bool parseTimeout(const std::string& value, int& timeoutMilliseconds);
} }
#endif
