// Hostname resolution for the Start paths (libquicsrt internal). Reuses the
// srt fork's own dial-time resolver - CreateAddr in deps/srt/apps/apputil.cpp,
// the helper srt-live-transmit uses - by compiling that file into libquicsrt
// (the fork's apps support library is otherwise not part of our build:
// ENABLE_APPS is OFF). CreateAddr tries inet_pton first (literal fast path,
// no resolver round trip) and falls back to the platform getaddrinfo:
// blocking with the OS resolver's default timeout budget, which is the
// accepted wait here (Start is blocking by design).

#ifndef QUICSRT_RESOLVE_H
#define QUICSRT_RESOLVE_H

#include <string>

namespace quicsrt
{

// Resolves `host` (an IPv4 literal or a DNS name) to an IPv4 dotted-quad
// string for the WT UDP dial. A literal passes through unchanged; a name
// resolves to its first A record. Returns the empty string with *error
// filled when the name does not resolve to an IPv4 address. Thread-safe:
// CreateAddr is a stateless getaddrinfo wrapper.

std::string ResolveHostToIpv4(const std::string& host, std::string* error);

} // namespace quicsrt

#endif // QUICSRT_RESOLVE_H
