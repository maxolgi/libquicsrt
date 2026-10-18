// subscriber_test: the libquicsrt public surface.
//
// (1) SubscriberConfig::validate() matrix - every field's bounds, plain
//     checks in the repo's style (no assert, Release-safe).
// (2) Offline API misuse: Read/Stats before Start, invalid config, Start
//     twice, destruction without Stop.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>

#include "quicsrt/subscriber.hpp"

namespace
{

std::atomic<bool> g_ok{true};

void Fail(const std::string& what)
{
    g_ok = false;
    std::fprintf(stderr, "subscriber_test: FAIL: %s\n", what.c_str());
}

int g_checks = 0;

// Plain checks (not assert) so the test stays meaningful in Release.
template <typename Cond>
void Check(const Cond& cond, const std::string& what)
{
    ++g_checks;
    if (!cond())
    {
        Fail(what);
    }
}

bool Contains(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
}

// ---- (1) config validation matrix ----

quicsrt::SubscriberConfig GoodConfig()
{
    quicsrt::SubscriberConfig cfg;
    cfg.host            = "127.0.0.1";
    cfg.port            = 4433;
    cfg.stream_name     = "subscriber_test";
    cfg.cert_sha256_hex = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    return cfg;
}

void ExpectValid(const quicsrt::SubscriberConfig& cfg, const std::string& what)
{
    Check([&] { return cfg.validate().empty(); }, what + " must validate");
}

void ExpectInvalid(const quicsrt::SubscriberConfig& cfg, const char* fragment, const std::string& what)
{
    const std::string error = cfg.validate();
    Check([&] { return !error.empty() && Contains(error, fragment); },
          what + " must be rejected (got \"" + error + "\")");
}

void TestConfigValidation()
{
    ExpectValid(GoodConfig(), "defaults + explicit good values");

    {
        auto cfg = GoodConfig();
        cfg.host = "";
        ExpectInvalid(cfg, "host", "empty host");
    }
    {
        auto cfg = GoodConfig();
        cfg.host = "localhost";
        ExpectValid(cfg, "DNS-name host");
    }
    {
        auto cfg = GoodConfig();
        cfg.host = "quicsrt.vyidd.com";
        ExpectValid(cfg, "multi-label DNS-name host");
    }
    {
        auto cfg = GoodConfig();
        cfg.host = "1.2.3.4.5";
        ExpectValid(cfg, "malformed IPv4 passes syntax as a (dead) DNS name - fails at Start resolution");
    }
    {
        auto cfg = GoodConfig();
        cfg.host = "256.1.1.1";
        ExpectValid(cfg, "out-of-range IPv4 passes syntax as a (dead) DNS name - fails at Start resolution");
    }
    {
        auto cfg = GoodConfig();
        cfg.host = "host with space";
        ExpectInvalid(cfg, "host", "whitespace in host");
    }
    {
        auto cfg = GoodConfig();
        cfg.host = "quicsrt://gw";
        ExpectInvalid(cfg, "host", "scheme in host");
    }
    {
        auto cfg = GoodConfig();
        cfg.host = "gw:4433";
        ExpectInvalid(cfg, "host", "colon (port syntax) in host");
    }
    {
        auto cfg = GoodConfig();
        cfg.host = "::1";
        ExpectInvalid(cfg, "host", "IPv6 literal host");
    }
    {
        auto cfg = GoodConfig();
        cfg.host = std::string(254, 'a');
        ExpectInvalid(cfg, "host", "254-character host");
    }
    {
        auto cfg = GoodConfig();
        cfg.host = std::string(253, 'a');
        ExpectValid(cfg, "253-character host (DNS length bound)");
    }
    {
        auto cfg = GoodConfig();
        cfg.port = 0;
        ExpectInvalid(cfg, "port", "port 0");
    }
    {
        auto cfg = GoodConfig();
        cfg.port = 65535;
        ExpectValid(cfg, "port 65535");
    }
    {
        auto cfg        = GoodConfig();
        cfg.stream_name = "";
        ExpectInvalid(cfg, "stream_name", "empty stream name");
    }
    {
        auto cfg        = GoodConfig();
        cfg.stream_name = "a b";
        ExpectInvalid(cfg, "stream_name", "space in stream name");
    }
    {
        auto cfg        = GoodConfig();
        cfg.stream_name = "a&b";
        ExpectInvalid(cfg, "stream_name", "ampersand in stream name");
    }
    {
        auto cfg        = GoodConfig();
        cfg.stream_name = "name#frag";
        ExpectInvalid(cfg, "stream_name", "hash in stream name");
    }
    {
        auto cfg        = GoodConfig();
        cfg.stream_name = "stream/1";
        ExpectInvalid(cfg, "stream_name", "slash in stream name");
    }
    {
        auto cfg        = GoodConfig();
        cfg.stream_name = "UPPER.lower_123~ok-x";
        ExpectValid(cfg, "unreserved charset stream name");
    }
    {
        auto cfg        = GoodConfig();
        cfg.stream_name = std::string(257, 'a');
        ExpectInvalid(cfg, "stream_name", "257-char stream name");
    }
    {
        auto cfg            = GoodConfig();
        cfg.cert_sha256_hex = "";
        ExpectValid(cfg, "empty cert hash (PKI mode)");
    }
    {
        auto cfg            = GoodConfig();
        cfg.cert_sha256_hex = "0123";
        ExpectInvalid(cfg, "cert_sha256_hex", "short cert hash");
    }
    {
        auto cfg            = GoodConfig();
        cfg.cert_sha256_hex = std::string(64, 'z');
        ExpectInvalid(cfg, "cert_sha256_hex", "non-hex cert hash");
    }
    {
        auto cfg            = GoodConfig();
        cfg.cert_sha256_hex = "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";
        ExpectValid(cfg, "uppercase cert hash");
    }
    {
        auto cfg             = GoodConfig();
        cfg.tsbpd_latency_ms = 0;
        ExpectInvalid(cfg, "tsbpd_latency_ms", "latency 0");
    }
    {
        auto cfg             = GoodConfig();
        cfg.tsbpd_latency_ms = 1;
        ExpectValid(cfg, "latency 1");
    }
    {
        auto cfg             = GoodConfig();
        cfg.tsbpd_latency_ms = 60001;
        ExpectInvalid(cfg, "tsbpd_latency_ms", "latency 60001");
    }
    {
        auto cfg         = GoodConfig();
        cfg.payload_size = 99;
        ExpectInvalid(cfg, "payload_size", "payload 99");
    }
    {
        auto cfg         = GoodConfig();
        cfg.payload_size = 187;
        ExpectInvalid(cfg, "payload_size", "payload 187");
    }
    {
        auto cfg         = GoodConfig();
        cfg.payload_size = 188;
        ExpectValid(cfg, "payload 188");
    }
    {
        auto cfg         = GoodConfig();
        cfg.payload_size = 1457;
        ExpectInvalid(cfg, "payload_size", "payload 1457");
    }
    {
        auto cfg         = GoodConfig();
        cfg.payload_size = 1456;
        ExpectValid(cfg, "payload 1456");
    }
    {
        auto cfg           = GoodConfig();
        cfg.initial_rtt_ms = -1;
        ExpectInvalid(cfg, "initial_rtt_ms", "negative rtt");
    }
    {
        auto cfg           = GoodConfig();
        cfg.initial_rtt_ms = 30001;
        ExpectInvalid(cfg, "initial_rtt_ms", "rtt 30001");
    }
    {
        auto cfg           = GoodConfig();
        cfg.initial_rtt_ms = 30000;
        ExpectValid(cfg, "rtt 30000");
    }
    {
        auto cfg               = GoodConfig();
        cfg.connect_timeout_ms = 99;
        ExpectInvalid(cfg, "connect_timeout_ms", "timeout 99");
    }
    {
        auto cfg               = GoodConfig();
        cfg.connect_timeout_ms = 600001;
        ExpectInvalid(cfg, "connect_timeout_ms", "timeout 600001");
    }
    {
        auto cfg               = GoodConfig();
        cfg.connect_timeout_ms = 100;
        ExpectValid(cfg, "timeout 100");
    }

    std::printf("config validation: matrix complete (%d checks so far)\n", g_checks);
}

// ---- (2) offline API misuse ----

void TestOfflineMisuse()
{
    {
        quicsrt::Subscriber sub;
        char               buf[1128];
        Check([&] { return sub.Read(buf, sizeof buf, 0) == quicsrt::kReadNotStarted; },
              "Read before Start must be kReadNotStarted");
        quicsrt::SubscriberStats stats;
        Check([&] { return sub.Stats(&stats) == quicsrt::kStatsNotConnected; }, "Stats before Start");
        Check([&] { return sub.LastError().empty(); }, "no error recorded yet");
    }
    {
        auto        cfg = GoodConfig();
        const auto* bad = &cfg;
        cfg.stream_name = "has space";
        quicsrt::Subscriber sub;
        Check([&] { return sub.Start(*bad) == quicsrt::kStartInvalidConfig; }, "Start with invalid config");
        Check([&] { return !sub.LastError().empty(); }, "invalid config records LastError");
        char buf[1128];
        Check([&] { return sub.Read(buf, sizeof buf, 0) == quicsrt::kReadNotStarted; },
              "Read after invalid-config Start");
        // The instance was not consumed by a failed validation: a corrected
        // config may still be started (it will fail at the transport here,
        // which is fine - the point is it is not kStartAlreadyStarted).
        cfg.stream_name        = "fixed";
        cfg.host               = "127.0.0.1";
        cfg.port               = 1; // nothing listens there
        cfg.connect_timeout_ms = 500;
        const auto status      = sub.Start(cfg);
        Check([&] { return status == quicsrt::kStartTimedOut || status == quicsrt::kStartConnectFailed; },
              "corrected-config Start must reach the transport, not kStartAlreadyStarted");
        Check([&] { return sub.Start(cfg) == quicsrt::kStartAlreadyStarted; }, "second Start is single-shot");
        sub.Stop();
        Check([&] { return sub.Read(buf, sizeof buf, 0) == quicsrt::kReadClosed; },
              "Read after failed Start is kReadClosed");
    }
    {
        // Unresolvable DNS name: typed, bounded Start failure (the .invalid
        // TLD is reserved for exactly this; NXDOMAIN latency is the local
        // resolver's, asserted bounded rather than fast).
        auto cfg               = GoodConfig();
        cfg.host               = "no-such-host.quicsrt-cpp.invalid";
        cfg.connect_timeout_ms = 500;
        const auto         t0  = std::chrono::steady_clock::now();
        quicsrt::Subscriber sub;
        const auto         status = sub.Start(cfg);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
        Check([&] { return status == quicsrt::kStartInvalidTarget; },
              "unresolvable host must kStartInvalidTarget (got " + std::to_string(status) + ")");
        Check([&] { return Contains(sub.LastError(), "resolve"); },
              "unresolvable host must name the resolution failure (got \"" + sub.LastError() + "\")");
        Check([&] { return ms.count() < 20000; },
              "unresolvable host must fail bounded (took " + std::to_string(ms.count()) + " ms)");
        char buf[1128];
        Check([&] { return sub.Read(buf, sizeof buf, 0) == quicsrt::kReadClosed; },
              "Read after resolve failure is kReadClosed");
        sub.Stop();
    }
    std::printf("offline misuse: complete (%d checks so far)\n", g_checks);
}

} // namespace

int main()
{
    TestConfigValidation();
    TestOfflineMisuse();

    if (!g_ok)
    {
        return 1;
    }
    std::printf("subscriber_test: ALL CHECKS PASSED (%d)\n", g_checks);
    return 0;
}
