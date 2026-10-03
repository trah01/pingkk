#ifndef PINGKK_DIAGNOSTICS_IPV6_H
#define PINGKK_DIAGNOSTICS_IPV6_H

#include "pingkk/diagnostics.h"

namespace pingkk {
bool sendIpv6Echo(const ResolvedTarget& target, int hopLimit,
                  int timeoutMilliseconds, unsigned short sequence,
                  PingResult& result, bool& hopExpired);
}

#endif
