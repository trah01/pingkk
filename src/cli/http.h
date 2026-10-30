#ifndef PINGKK_CLI_HTTP_H
#define PINGKK_CLI_HTTP_H

#include "pingkk/core.h"
#include <functional>
#include <string>

namespace pingkk { namespace cli {
int runHttpCheck(const std::string& url, int timeoutMilliseconds, bool advanced,
                 IpVersion version, const std::function<bool()>& keepRunning,
                 const std::string& proxy = std::string());
} }

#endif
