// publisher_test: the libwebsrt Publisher surface.
//
// (1) PublisherConfig::validate() matrix - the shared bounds.
// (2) TsChunker units: 188 alignment, exact 1128 boundaries, sub-188
//     remainder carryover, non-multiple payload sizes, TruncateTo.
// (2b) Start HTTP-status mapping units (the façades' rejected vs
//     connect-failed seam): 403/404/429 are the typed refusals, 5xx and
//     the unparseable -1 are transport failures.
// (3) Offline API misuse: Write/Stats/End before Start, invalid config,
//     Start twice, destruction without Stop.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "srt_session/publisher.h"
#include "websrt/publisher.hpp"
#include "websrt/start_status.h"

namespace
{

std::atomic<bool> g_ok{true};

void Fail(const std::string& what)
{
    g_ok = false;
    std::fprintf(stderr, "publisher_test: FAIL: %s\n", what.c_str());
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

websrt::PublisherConfig GoodConfig()
{
    websrt::PublisherConfig cfg;
    cfg.host            = "127.0.0.1";
    cfg.port            = 4433;
    cfg.stream_name     = "publisher_test";
    cfg.cert_sha256_hex = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    return cfg;
}

void ExpectValid(const websrt::PublisherConfig& cfg, const std::string& what)
{
    Check([&] { return cfg.validate().empty(); }, what + " must validate");
}

void ExpectInvalid(const websrt::PublisherConfig& cfg, const char* fragment, const std::string& what)
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
        cfg.host = "256.9.9.9";
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
        cfg.host = "::1";
        ExpectInvalid(cfg, "host", "IPv6 literal host");
    }
    {
        auto cfg = GoodConfig();
        cfg.port = 0;
        ExpectInvalid(cfg, "port", "port 0");
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
        auto cfg            = GoodConfig();
        cfg.cert_sha256_hex = "0123";
        ExpectInvalid(cfg, "cert_sha256_hex", "short cert hash");
    }
    {
        auto cfg            = GoodConfig();
        cfg.cert_sha256_hex = "";
        ExpectValid(cfg, "empty cert hash (PKI mode)");
    }
    {
        auto cfg             = GoodConfig();
        cfg.tsbpd_latency_ms = 0;
        ExpectInvalid(cfg, "tsbpd_latency_ms", "latency 0");
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
        auto cfg           = GoodConfig();
        cfg.initial_rtt_ms = 30001;
        ExpectInvalid(cfg, "initial_rtt_ms", "rtt 30001");
    }
    {
        auto cfg               = GoodConfig();
        cfg.connect_timeout_ms = 99;
        ExpectInvalid(cfg, "connect_timeout_ms", "timeout 99");
    }

    std::printf("config validation: matrix complete (%d checks so far)\n", g_checks);
}

// ---- (2) TsChunker units ----

std::vector<std::uint8_t> Bytes(int n, std::uint8_t seed)
{
    std::vector<std::uint8_t> v(static_cast<std::size_t>(n));
    for (std::size_t i = 0; i < v.size(); ++i)
    {
        v[i] = static_cast<std::uint8_t>(seed + i);
    }
    return v;
}

void TestChunker()
{
    using websrt::TsChunker;
    constexpr int kTs = 188;

    {
        TsChunker c(1128);
        Check([&] { return c.NextChunkLen() == 0; }, "empty chunker emits nothing");
        Check([&] { return c.pending() == 0; }, "empty pending");
    }
    { // sub-188 remainder carries over and completes with the next Feed
        TsChunker c(1128);
        c.Feed(Bytes(100, 1).data(), 100);
        Check([&] { return c.NextChunkLen() == 0; }, "100 bytes: no complete TS packet");
        Check([&] { return c.pending() == 100; }, "100 bytes buffered");
        c.Feed(Bytes(88, 2).data(), 88);
        Check([&] { return c.pending() == 188; }, "carried to exactly one TS packet");
        Check([&] { return c.NextChunkLen() == 188; }, "one TS packet emitted");
        c.Consume(188);
        Check([&] { return c.pending() == 0; }, "consumed to empty");
    }
    { // 7 x 188 (a 1316-byte Write) frames as 1128 + 188
        TsChunker  c(1128);
        const auto data = Bytes(7 * kTs, 3);
        c.Feed(data.data(), data.size());
        Check([&] { return c.NextChunkLen() == 1128; }, "first message is the full 1128");
        c.Consume(1128);
        Check([&] { return c.NextChunkLen() == 188; }, "second message is the 188 remainder");
        c.Consume(188);
        Check([&] { return c.NextChunkLen() == 0; }, "7 x 188 drained exactly");
    }
    { // exact 1128 boundary
        TsChunker  c(1128);
        const auto data = Bytes(1128, 4);
        c.Feed(data.data(), data.size());
        Check([&] { return c.NextChunkLen() == 1128; }, "exact 1128 emits one full message");
        c.Consume(1128);
        Check([&] { return c.NextChunkLen() == 0; }, "nothing behind the boundary");
    }
    { // 1129 leaves the 1-byte carry
        TsChunker  c(1128);
        const auto data = Bytes(1129, 5);
        c.Feed(data.data(), data.size());
        Check([&] { return c.NextChunkLen() == 1128; }, "1129 still emits 1128 first");
        c.Consume(1128);
        Check([&] { return c.pending() == 1 && c.NextChunkLen() == 0; }, "1-byte carry, nothing emittable");
    }
    { // payload not a multiple of 188: messages floor to whole packets
        TsChunker  c(1000);
        const auto data = Bytes(2000, 6);
        c.Feed(data.data(), data.size());
        Check([&] { return c.NextChunkLen() == 5 * kTs; }, "payload 1000 -> 940-byte messages");
        c.Consume(5 * kTs);
        Check([&] { return c.NextChunkLen() == 5 * kTs; }, "second 940");
        c.Consume(5 * kTs);
        Check([&] { return c.pending() == 2000 - 10 * kTs; }, "non-multiple tail carried");
    }
    { // alignment identity over random-ish feeds: every emitted message
      // is a 188 multiple <= payload, the stream content is preserved in
      // order, and only a sub-188 tail stays behind.
        TsChunker                 c(1128);
        std::vector<std::uint8_t> stream;
        std::vector<std::uint8_t> framed;
        std::uint32_t             rng = 20261012;
        for (int i = 0; i < 200; ++i)
        {
            rng              = rng * 1103515245U + 12345U;
            const int  n     = 1 + static_cast<int>(rng % 5000);
            const auto piece = Bytes(n, static_cast<std::uint8_t>(i));
            stream.insert(stream.end(), piece.begin(), piece.end());
            c.Feed(piece.data(), piece.size());
            for (;;)
            {
                const int n2 = c.NextChunkLen();
                if (n2 == 0)
                {
                    break;
                }
                Check([&] { return n2 % kTs == 0 && n2 <= 1128; }, "emitted message is 188-aligned and bounded");
                framed.insert(framed.end(), c.data(), c.data() + n2);
                c.Consume(n2);
            }
            Check([&] { return c.pending() < static_cast<std::size_t>(kTs); }, "carry stays sub-188");
        }
        Check([&] { return framed.size() == stream.size() - stream.size() % kTs; },
              "framed bytes == whole TS packets fed");
        Check([&] { return std::memcmp(framed.data(), stream.data(), framed.size()) == 0; },
              "framing preserves byte order");
    }
    { // TruncateTo rolls a Feed back (the Write failure path's primitive)
        TsChunker  c(1128);
        const auto keep = Bytes(150, 7);
        c.Feed(keep.data(), keep.size());
        const auto extra = Bytes(300, 8);
        c.Feed(extra.data(), extra.size());
        c.TruncateTo(150);
        Check([&] { return c.pending() == 150; }, "TruncateTo drops the later Feed");
        Check([&] { return c.NextChunkLen() == 0 && std::memcmp(c.data(), keep.data(), 150) == 0; },
              "carried bytes intact after rollback");
    }
    { // the partial-Write accounting shape: an accepted carry consumed
      // by a send, then the unsent new bytes handed back via TruncateTo.
        TsChunker  c(1128);
        const auto carry = Bytes(188, 9);
        c.Feed(carry.data(), carry.size());
        const auto extra = Bytes(100, 10);
        c.Feed(extra.data(), extra.size());
        Check([&] { return c.NextChunkLen() == 188; }, "carry-first emission");
        c.Consume(188);  // the send that succeeded
        c.TruncateTo(0); // keep = max(188 - 188, 0)
        Check([&] { return c.pending() == 0; }, "unsent new bytes returned to the caller");
    }

    std::printf("chunker units: complete (%d checks so far)\n", g_checks);
}

// ---- (2b) Start HTTP-status mapping units ----

// The façades' kXRejected / kXConnectFailed seam (websrt/start_status.h):
// 403/404/429 are the typed gateway refusals, 5xx and the unparseable -1
// are transport failures - a pure mapping, asserted offline.
void TestHttpStatusMapping()
{
    using websrt::HttpStatusIsRejected;
    Check([&] { return HttpStatusIsRejected(403) && HttpStatusIsRejected(404) && HttpStatusIsRejected(429); },
          "403/404/429 are the typed gateway refusals");
    Check([&] { return !HttpStatusIsRejected(500) && !HttpStatusIsRejected(502) && !HttpStatusIsRejected(503); },
          "5xx are gateway failures, not refusals");
    Check([&] { return !HttpStatusIsRejected(-1) && !HttpStatusIsRejected(200); },
          "-1 (unparseable) and 200 are not refusals");
    Check([&] { return !HttpStatusIsRejected(302) && !HttpStatusIsRejected(400); },
          "other statuses (302, 400) are not refusals");
    std::printf("status mapping units: complete (%d checks so far)\n", g_checks);
}

// ---- (3) offline API misuse ----

void TestOfflineMisuse()
{
    {
        websrt::Publisher pub;
        char              buf[1128];
        Check([&] { return pub.Write(buf, sizeof buf, 0) == websrt::kWriteNotStarted; },
              "Write before Start must be kWriteNotStarted");
        websrt::PublisherStats stats;
        Check([&] { return pub.Stats(&stats) == websrt::kStatsNotConnected; }, "Stats before Start");
        Check([&] { return !pub.End(100); }, "End before Start fails");
        Check([&] { return pub.LastError().empty(); }, "no error recorded yet");
    }
    {
        auto        cfg = GoodConfig();
        const auto* bad = &cfg;
        cfg.stream_name = "has space";
        websrt::Publisher pub;
        Check([&] { return pub.Start(*bad) == websrt::kPublishInvalidConfig; }, "Start with invalid config");
        Check([&] { return !pub.LastError().empty(); }, "invalid config records LastError");
        char buf[1128];
        Check([&] { return pub.Write(buf, sizeof buf, 0) == websrt::kWriteNotStarted; },
              "Write after invalid-config Start");
        // The instance was not consumed by a failed validation: a
        // corrected config may still be started (fails at the transport
        // here - the point is it is not kPublishAlreadyStarted).
        cfg.stream_name        = "fixed";
        cfg.host               = "127.0.0.1";
        cfg.port               = 1; // nothing listens there
        cfg.connect_timeout_ms = 500;
        const auto status      = pub.Start(cfg);
        Check([&] { return status == websrt::kPublishTimedOut || status == websrt::kPublishConnectFailed; },
              "corrected-config Start must reach the transport, not kPublishAlreadyStarted");
        Check([&] { return pub.Start(cfg) == websrt::kPublishAlreadyStarted; }, "second Start is single-shot");
        Check([&] { return pub.Write(buf, sizeof buf, 0) == websrt::kWriteClosed; },
              "Write after failed Start is kWriteClosed");
        pub.Stop();
        Check([&] { return pub.Write(buf, sizeof buf, 0) == websrt::kWriteClosed; }, "Write stays terminal after Stop");
    }
    {
        // Unresolvable DNS name: typed, bounded Start failure (same
        // discipline as the Subscriber façade's case).
        auto cfg               = GoodConfig();
        cfg.host               = "no-such-host.websrt-cpp.invalid";
        cfg.connect_timeout_ms = 500;
        const auto        t0   = std::chrono::steady_clock::now();
        websrt::Publisher pub;
        const auto        status = pub.Start(cfg);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
        Check([&] { return status == websrt::kPublishInvalidTarget; },
              "unresolvable host must kPublishInvalidTarget (got " + std::to_string(status) + ")");
        Check([&] { return Contains(pub.LastError(), "resolve"); },
              "unresolvable host must name the resolution failure (got \"" + pub.LastError() + "\")");
        Check([&] { return ms.count() < 20000; },
              "unresolvable host must fail bounded (took " + std::to_string(ms.count()) + " ms)");
        char buf[1128];
        Check([&] { return pub.Write(buf, sizeof buf, 0) == websrt::kWriteClosed; },
              "Write after resolve failure is kWriteClosed");
        pub.Stop();
    }
    std::printf("offline misuse: complete (%d checks so far)\n", g_checks);
}

} // namespace

int main()
{
    TestConfigValidation();
    TestChunker();
    TestHttpStatusMapping();
    TestOfflineMisuse();

    if (!g_ok)
    {
        return 1;
    }
    std::printf("publisher_test: ALL CHECKS PASSED (%d)\n", g_checks);
    return 0;
}
