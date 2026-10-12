// subscriber_test: the libwebsrt public surface.
//
// (1) SubscriberConfig::validate() matrix - every field's bounds, plain
//     checks in the repo's style (no assert, Release-safe).
// (2) Offline API misuse: Read/Stats before Start, invalid config, Start
//     twice, destruction without Stop.
// (3) Live negative path against the local Rust gateway oracle (the
//     supervisord `websrt` service + its runtime cert-hash.js): a
//     deliberately nonexistent stream gets HTTP 404 pre-accept, so Start
//     must surface kStartRejected, Read the documented kReadClosed, and
//     shutdown must be hang-free; a fresh Subscriber must repeat the
//     dance cleanly. A dead-port target must fail bounded instead of
//     hanging. Exits 77 (ctest SKIP_RETURN_CODE) when the oracle is
//     absent so CI without the Rust world stays green.
//
// The positive path (dumping a live published stream) is the next
// agent's gate (websrt-dump, P1): it needs a published-stream fixture.

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <thread>
#include <string>

#include "websrt/subscriber.hpp"

namespace
{

constexpr int kSkipExit = 77;

constexpr char kDefaultGateway[] = "127.0.0.1:4433";

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

// cert-hash.js: `window.CERT_HASH = "<64 hex chars>";` (self-signed mode).
// Same loader contract as wt_echo: nullopt when the file is missing, the
// hash is null (mkcert mode), or the value does not parse.
std::optional<std::string> LoadCertHashHex()
{
    constexpr std::size_t kSha256HexLen = 64;
    const char*           path          = std::getenv("WEBSRT_CERT_HASH_JS");
    if (path == nullptr || *path == '\0')
    {
        path = WEBSRT_CERT_HASH_JS_DEFAULT;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        return std::nullopt;
    }
    const std::string js((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    const std::size_t key = js.find("CERT_HASH");
    if (key == std::string::npos)
    {
        return std::nullopt;
    }
    const std::size_t quote = js.find('"', key);
    if (quote == std::string::npos)
    {
        return std::nullopt;
    }
    if (js.size() < quote + 1 + kSha256HexLen)
    {
        return std::nullopt;
    }
    std::string        hex        = js.substr(quote + 1, kSha256HexLen);
    static const char* kHexDigits = "0123456789abcdefABCDEF";
    for (const char c : hex)
    {
        if (std::strchr(kHexDigits, c) == nullptr)
        {
            return std::nullopt;
        }
    }
    return hex;
}

// ---- (1) config validation matrix ----

websrt::SubscriberConfig GoodConfig()
{
    websrt::SubscriberConfig cfg;
    cfg.host            = "127.0.0.1";
    cfg.port            = 4433;
    cfg.stream_name     = "subscriber_test";
    cfg.cert_sha256_hex = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    return cfg;
}

void ExpectValid(const websrt::SubscriberConfig& cfg, const std::string& what)
{
    Check([&] { return cfg.validate().empty(); }, what + " must validate");
}

void ExpectInvalid(const websrt::SubscriberConfig& cfg, const char* fragment, const std::string& what)
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
        cfg.host = "websrt.vyidd.com";
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
        cfg.host = "websrt://gw";
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
        cfg.payload_size = 100;
        ExpectValid(cfg, "payload 100");
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
        websrt::Subscriber sub;
        char               buf[1128];
        Check([&] { return sub.Read(buf, sizeof buf, 0) == websrt::kReadNotStarted; },
              "Read before Start must be kReadNotStarted");
        websrt::SubscriberStats stats;
        Check([&] { return sub.Stats(&stats) == websrt::kStatsNotConnected; }, "Stats before Start");
        Check([&] { return sub.LastError().empty(); }, "no error recorded yet");
    }
    {
        auto        cfg = GoodConfig();
        const auto* bad = &cfg;
        cfg.stream_name = "has space";
        websrt::Subscriber sub;
        Check([&] { return sub.Start(*bad) == websrt::kStartInvalidConfig; }, "Start with invalid config");
        Check([&] { return !sub.LastError().empty(); }, "invalid config records LastError");
        char buf[1128];
        Check([&] { return sub.Read(buf, sizeof buf, 0) == websrt::kReadNotStarted; },
              "Read after invalid-config Start");
        // The instance was not consumed by a failed validation: a corrected
        // config may still be started (it will fail at the transport here,
        // which is fine - the point is it is not kStartAlreadyStarted).
        cfg.stream_name        = "fixed";
        cfg.host               = "127.0.0.1";
        cfg.port               = 1; // nothing listens there
        cfg.connect_timeout_ms = 500;
        const auto status      = sub.Start(cfg);
        Check([&] { return status == websrt::kStartTimedOut || status == websrt::kStartConnectFailed; },
              "corrected-config Start must reach the transport, not kStartAlreadyStarted");
        Check([&] { return sub.Start(cfg) == websrt::kStartAlreadyStarted; }, "second Start is single-shot");
        sub.Stop();
        Check([&] { return sub.Read(buf, sizeof buf, 0) == websrt::kReadClosed; },
              "Read after failed Start is kReadClosed");
    }
    {
        // Unresolvable DNS name: typed, bounded Start failure (the .invalid
        // TLD is reserved for exactly this; NXDOMAIN latency is the local
        // resolver's, asserted bounded rather than fast).
        auto cfg               = GoodConfig();
        cfg.host               = "no-such-host.websrt-cpp.invalid";
        cfg.connect_timeout_ms = 500;
        const auto         t0  = std::chrono::steady_clock::now();
        websrt::Subscriber sub;
        const auto         status = sub.Start(cfg);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
        Check([&] { return status == websrt::kStartInvalidTarget; },
              "unresolvable host must kStartInvalidTarget (got " + std::to_string(status) + ")");
        Check([&] { return Contains(sub.LastError(), "resolve"); },
              "unresolvable host must name the resolution failure (got \"" + sub.LastError() + "\")");
        Check([&] { return ms.count() < 20000; },
              "unresolvable host must fail bounded (took " + std::to_string(ms.count()) + " ms)");
        char buf[1128];
        Check([&] { return sub.Read(buf, sizeof buf, 0) == websrt::kReadClosed; },
              "Read after resolve failure is kReadClosed");
        sub.Stop();
    }
    std::printf("offline misuse: complete (%d checks so far)\n", g_checks);
}

// ---- (3) live negative path ----

// One live negative-path Start, with one retry. A kStartTimedOut against
// the real gateway is the deaf-oracle signature (the Rust gateway's accept
// loop intermittently starves for a couple of minutes under churn and then
// self-heals; wt_echo's "no UDP life" SKIP covers the same outage), so it
// is retried once after a pause and then surfaces as SKIP rather than as
// a client failure. Any other status is final.
int StartNegative(const websrt::SubscriberConfig& cfg)
{
    int status = websrt::kStartTimedOut;
    for (int attempt = 0; attempt < 2 && status == websrt::kStartTimedOut; ++attempt)
    {
        if (attempt > 0)
        {
            std::printf("live negative: gateway unresponsive, retrying in 3 s\n");
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
        websrt::Subscriber sub;
        status = sub.Start(cfg);
        char buf[1128];
        if (status == websrt::kStartTimedOut)
        {
            sub.Stop();
            continue;
        }
        // Outside the deaf-oracle case the session is fully judged here.
        Check([&] { return status == websrt::kStartRejected; },
              "nonexistent stream must kStartRejected (got " + std::to_string(status) + ")");
        const std::string error = sub.LastError();
        Check([&] { return Contains(error, "404") || Contains(error, "HTTP"); },
              "LastError must mention the HTTP rejection (got \"" + error + "\")");
        Check([&] { return sub.Read(buf, sizeof buf, 1000) == websrt::kReadClosed; },
              "Read after rejection must be kReadClosed");
        websrt::SubscriberStats stats;
        Check([&] { return sub.Stats(&stats) == websrt::kStatsNotConnected; }, "Stats after rejection");
        const auto t0 = std::chrono::steady_clock::now();
        sub.Stop();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
        Check([&] { return ms.count() < 2000; }, "Stop after rejection must be fast");
        Check([&] { return sub.Read(buf, sizeof buf, 0) == websrt::kReadClosed; }, "Read stays terminal after Stop");
    }
    return status;
}

bool GatewayTarget(websrt::SubscriberConfig* cfg, const std::string& stream_suffix)
{
    const char*       gateway_env = std::getenv("WEBSRT_WT_GATEWAY");
    const std::string gateway     = gateway_env != nullptr && *gateway_env != '\0' ? gateway_env : kDefaultGateway;
    const std::size_t colon       = gateway.rfind(':');
    if (colon == std::string::npos)
    {
        return false;
    }
    const auto hash = LoadCertHashHex();
    if (!hash)
    {
        return false;
    }
    cfg->host            = gateway.substr(0, colon);
    cfg->port            = static_cast<std::uint16_t>(std::atoi(gateway.c_str() + colon + 1));
    cfg->stream_name     = "websrt-cpp-negative-" + stream_suffix + "-" + std::to_string(::getpid());
    cfg->cert_sha256_hex = *hash;
    return true;
}

void TestLiveNegative()
{
    const auto t0 = std::chrono::steady_clock::now();

    // (a) Nonexistent stream: the gateway answers HTTP 404 before the WT
    // session is ever accepted - Start must surface the rejection.
    {
        websrt::SubscriberConfig cfg;
        if (!GatewayTarget(&cfg, "one"))
        {
            std::printf("subscriber_test: SKIP: oracle target unavailable\n");
            std::exit(kSkipExit);
        }
        cfg.connect_timeout_ms = 5000;
        if (StartNegative(cfg) == websrt::kStartTimedOut)
        {
            std::printf("subscriber_test: SKIP: gateway oracle unresponsive\n");
            std::exit(kSkipExit);
        }
    }

    // (b) Fresh instance, same dance: no cross-instance residue (srt
    // library refcount, loop threads, WT engine).
    {
        websrt::SubscriberConfig cfg;
        if (!GatewayTarget(&cfg, "two"))
        {
            std::exit(kSkipExit);
        }
        cfg.connect_timeout_ms = 5000;
        if (StartNegative(cfg) == websrt::kStartTimedOut)
        {
            std::exit(kSkipExit);
        }
    }

    // (c) Dead port: bounded failure instead of a hang.
    {
        const auto hash = LoadCertHashHex();
        if (!hash)
        {
            std::exit(kSkipExit);
        }
        websrt::SubscriberConfig cfg = GoodConfig();
        cfg.host                     = "127.0.0.1";
        cfg.port                     = 1; // nothing listens on UDP 1
        cfg.connect_timeout_ms       = 1500;
        cfg.cert_sha256_hex          = *hash;

        const auto         dead_t0 = std::chrono::steady_clock::now();
        websrt::Subscriber sub;
        const auto         status = sub.Start(cfg);
        const auto         dead_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - dead_t0);
        Check([&] { return status == websrt::kStartTimedOut || status == websrt::kStartConnectFailed; },
              "dead port must fail bounded (got " + std::to_string(status) + ")");
        Check([&] { return dead_ms.count() >= 1400 && dead_ms.count() < 10000; },
              "dead-port Start must respect connect_timeout_ms (took " + std::to_string(dead_ms.count()) + " ms)");
        char buf[1128];
        Check([&] { return sub.Read(buf, sizeof buf, 100) == websrt::kReadClosed; }, "Read after dead-port failure");
        sub.Stop();
    }

    // (d) PKI mode (empty cert hash) against the self-signed local
    // gateway: the chain is not in the system CA bundle, so the TLS
    // handshake must fail and Start must surface kStartConnectFailed.
    {
        websrt::SubscriberConfig cfg;
        if (!GatewayTarget(&cfg, "pki"))
        {
            std::exit(kSkipExit);
        }
        cfg.cert_sha256_hex    = ""; // PKI validation instead of pinning
        cfg.connect_timeout_ms = 5000;

        websrt::Subscriber sub;
        const auto         status = sub.Start(cfg);
        Check([&] { return status == websrt::kStartConnectFailed; },
              "PKI mode must reject the self-signed gateway (got " + std::to_string(status) + ")");
        Check([&] { return Contains(sub.LastError(), "TLS"); }, "PKI rejection must name the TLS failure");
        char buf[1128];
        Check([&] { return sub.Read(buf, sizeof buf, 100) == websrt::kReadClosed; }, "Read after PKI rejection");
        sub.Stop();

        // The client-side TLS abort leaves the gateway's serialized accept
        // loop waiting out its ~10s session-establishment timeout; until
        // it drains, the oracle stalls every incoming connection (the
        // gateway's known transient, it self-heals). Cool down so this
        // test leaves a healthy gateway behind for whatever runs next.
        std::printf("live negative: cooling down 12 s after the PKI abort\n");
        std::this_thread::sleep_for(std::chrono::seconds(12));
    }

    // (e) Hostname dial: bare "localhost" with pinning. The TLS handshake
    // must succeed against the name-resolved gateway (SNI carries the
    // typed name, the pin still matches the cert), reaching the HTTP 404
    // for the nonexistent stream - resolution + SNI-with-name + pinning
    // coexist.
    {
        websrt::SubscriberConfig cfg;
        if (!GatewayTarget(&cfg, "byname"))
        {
            std::exit(kSkipExit);
        }
        cfg.host               = "localhost";
        cfg.connect_timeout_ms = 5000;
        if (StartNegative(cfg) == websrt::kStartTimedOut)
        {
            std::exit(kSkipExit);
        }
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
    std::printf("live negative path: complete in %lld ms (%d checks so far)\n",
                static_cast<long long>(elapsed.count()),
                g_checks);
}

} // namespace

int main()
{
    TestConfigValidation();
    TestOfflineMisuse();
    TestLiveNegative();

    if (!g_ok)
    {
        return 1;
    }
    std::printf("subscriber_test: ALL CHECKS PASSED (%d)\n", g_checks);
    return 0;
}
