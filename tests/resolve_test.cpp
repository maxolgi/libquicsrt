// resolve_test: the resolver reused from the srt fork (CreateAddr,
// deps/srt/apps/apputil.cpp) behind ResolveHostToIpv4.
//
// (1) IPv4 literals pass through unchanged (CreateAddr's inet_pton fast
//     path - no resolver round trip).
// (2) "localhost" resolves to a loopback address (asserted loopback, not
//     a specific family or spelling).
// (3) An RFC 6761 .invalid name fails with a descriptive error, bounded
//     by the local resolver's NXDOMAIN latency.

#include <arpa/inet.h>
#include <netinet/in.h>

#include <chrono>
#include <cstdio>
#include <string>

#include "websrt/resolve.h"

namespace
{

int  g_checks = 0;
bool g_ok     = true;

template <typename Cond>
void Check(const Cond& cond, const std::string& what)
{
    ++g_checks;
    if (!cond())
    {
        g_ok = false;
        std::fprintf(stderr, "resolve_test: FAIL: %s\n", what.c_str());
    }
}

bool IsIpv4Loopback(const std::string& dotted)
{
    in_addr a{};
    if (inet_pton(AF_INET, dotted.c_str(), &a) != 1)
    {
        return false;
    }
    return (ntohl(a.s_addr) >> 24) == IN_LOOPBACKNET;
}

} // namespace

int main()
{
    {
        std::string err = "unset";
        const auto  ip  = websrt::ResolveHostToIpv4("127.0.0.1", &err);
        Check([&] { return ip == "127.0.0.1"; }, "literal passthrough (got \"" + ip + "\")");
        Check([&] { return err == "unset"; }, "literal passthrough leaves the error untouched");
    }
    {
        const auto ip = websrt::ResolveHostToIpv4("192.168.1.2", nullptr);
        Check([&] { return ip == "192.168.1.2"; }, "second literal passthrough");
    }
    {
        std::string err;
        const auto  ip = websrt::ResolveHostToIpv4("localhost", &err);
        Check([&] { return !ip.empty(); }, "localhost must resolve (error: \"" + err + "\")");
        Check([&] { return IsIpv4Loopback(ip); }, "localhost must be loopback (got " + ip + ")");
    }
    {
        const auto  t0 = std::chrono::steady_clock::now();
        std::string err;
        const auto  ip = websrt::ResolveHostToIpv4("no-such-host.websrt-cpp.invalid", &err);
        const auto  ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
        Check([&] { return ip.empty(); }, ".invalid name must not resolve (got " + ip + ")");
        Check([&] { return err.find("no-such-host.websrt-cpp.invalid") != std::string::npos; },
              "error must name the host (got \"" + err + "\")");
        Check([&] { return ms.count() < 20000; },
              "NXDOMAIN must come back bounded (took " + std::to_string(ms.count()) + " ms)");
    }

    if (!g_ok)
    {
        return 1;
    }
    std::printf("resolve_test: ALL CHECKS PASSED (%d)\n", g_checks);
    return 0;
}
