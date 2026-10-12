// publisher_test: the libwebsrt Publisher surface.
//
// (1) PublisherConfig::validate() matrix - the shared bounds.
// (2) TsChunker units: 188 alignment, exact 1128 boundaries, sub-188
//     remainder carryover, non-multiple payload sizes, TruncateTo.
// (3) Offline API misuse: Write/Stats/End before Start, invalid config,
//     Start twice, destruction without Stop.
// (4) LIVE GATE against the local Rust gateway oracle (the supervisord
//     `websrt` service + its runtime cert-hash.js + fixtures/test.ts):
//     a Publisher feeds the fixture once (varied chunk sizes, ~10 s
//     pace, 2 s pre-grace like scripts/interop.sh's publisher) while an
//     in-process Subscriber (wait-retry) dumps the stream; the dump must
//     equal the fixture byte-for-byte, both sides' stats must show the
//     stream survived, a mid-stream challenger for the same name must
//     get the typed first-wins rejection (HTTP 403), and the name must
//     free after End so a fresh Start succeeds. Exits 77 (ctest
//     SKIP_RETURN_CODE) when the oracle is absent.

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "srt_session/publisher.h"
#include "websrt/publisher.hpp"
#include "websrt/subscriber.hpp"

namespace
{

constexpr int kSkipExit = 77;

constexpr char kDefaultGateway[] = "127.0.0.1:4433";

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

// cert-hash.js: `window.CERT_HASH = "<64 hex chars>";` (self-signed mode).
// Same loader contract as subscriber_test: nullopt when the file is
// missing, the hash is null (mkcert mode), or the value does not parse.
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
        ExpectInvalid(cfg, "host", "DNS-name host");
    }
    {
        auto cfg = GoodConfig();
        cfg.host = "256.9.9.9";
        ExpectInvalid(cfg, "host", "out-of-range IPv4");
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
    std::printf("offline misuse: complete (%d checks so far)\n", g_checks);
}

// ---- (4) live gate ----

std::optional<std::vector<std::uint8_t>> LoadFixture()
{
    const char* path = std::getenv("WEBSRT_TEST_TS");
    if (path == nullptr || *path == '\0')
    {
        path = WEBSRT_FIXTURE_TS_DEFAULT;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        return std::nullopt;
    }
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.empty() || data.size() % 188 != 0)
    {
        return std::nullopt;
    }
    return data;
}

bool GatewayTarget(websrt::PublisherConfig* cfg, const std::string& stream_name)
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
    cfg->stream_name     = stream_name;
    cfg->cert_sha256_hex = *hash;
    return true;
}

// The deaf-oracle signature (the Rust gateway's accept loop starving for
// a couple of minutes under churn, then self-healing) is a kPublishTimedOut
// Start; one retry with a fresh instance (a Publisher is single-shot),
// then SKIP. On success *out holds the live publisher.
websrt::PublisherStartStatus StartWithRetry(const websrt::PublisherConfig& cfg, std::unique_ptr<websrt::Publisher>* out)
{
    websrt::PublisherStartStatus status = websrt::kPublishTimedOut;
    for (int attempt = 0; attempt < 2 && status == websrt::kPublishTimedOut; ++attempt)
    {
        if (attempt > 0)
        {
            std::printf("live gate: gateway unresponsive, retrying in 3 s\n");
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
        *out   = std::make_unique<websrt::Publisher>();
        status = (*out)->Start(cfg);
    }
    return status;
}

void TestLiveGate()
{
    const auto fixture = LoadFixture();
    if (!fixture)
    {
        std::printf("publisher_test: SKIP: fixture unavailable\n");
        std::exit(kSkipExit);
    }

    const std::string       name = "websrt-cpp-pub-" + std::to_string(::getpid());
    websrt::PublisherConfig cfg;
    if (!GatewayTarget(&cfg, name))
    {
        std::printf("publisher_test: SKIP: oracle target unavailable\n");
        std::exit(kSkipExit);
    }
    cfg.connect_timeout_ms = 5000;

    const auto t0 = std::chrono::steady_clock::now();

    // (a) Publisher starts: gateway 200s the ?publish= CONNECT and then
    // initiates SRT (Conclusion flood) against our listener.
    std::unique_ptr<websrt::Publisher> pub;
    const auto                         status = StartWithRetry(cfg, &pub);
    if (status == websrt::kPublishTimedOut)
    {
        std::printf("publisher_test: SKIP: gateway oracle unresponsive\n");
        std::exit(kSkipExit);
    }
    Check([&] { return status == websrt::kPublishOk; },
          "Publisher Start must succeed against the oracle (got " + std::to_string(status) + ": " + pub->LastError() +
              ")");

    // (b) In-process subscriber with wait-retry: fresh instances while
    // the gateway would answer 404 (here it should attach immediately -
    // the publisher registered the name at CONNECT-accept time).
    std::optional<std::vector<std::uint8_t>> dumped;
    websrt::SubscriberStats                  sub_stats_mid{};
    {
        bool attached = false;
        for (int attempt = 0; attempt < 20 && !attached; ++attempt)
        {
            if (attempt > 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            websrt::SubscriberConfig scfg;
            scfg.host               = cfg.host;
            scfg.port               = cfg.port;
            scfg.stream_name        = name;
            scfg.cert_sha256_hex    = cfg.cert_sha256_hex;
            scfg.connect_timeout_ms = 5000;

            websrt::Subscriber sub;
            const auto         s = sub.Start(scfg);
            if (s == websrt::kStartRejected)
            {
                continue; // name not visible yet (or gateway churn)
            }
            if (s != websrt::kStartOk)
            {
                Fail("subscriber wait-retry Start failed (got " + std::to_string(s) + ")");
                break;
            }

            // (c) Pre-grace: the gateway broadcasts only to receivers
            // that exist when a message is sent - attach before byte 0
            // (ingest_send's 2 s discipline).
            std::this_thread::sleep_for(std::chrono::seconds(2));

            // (d) Feed the fixture once, paced over ~10 s, in varied
            // chunk sizes (the chunker normalizes to 1128s). At the
            // midpoint, prove first-wins occupancy: a challenger for
            // the same name gets the typed rejection.
            const double      bytes_per_sec   = static_cast<double>(fixture->size()) / 10.0;
            const auto        feed_t0         = std::chrono::steady_clock::now();
            std::size_t       off             = 0;
            std::size_t       pattern_idx     = 0;
            bool              challenger_done = false;
            std::uint32_t     rng             = 42;
            const std::size_t pattern[]       = {std::size_t{7} * 188, 4096, 2048, 700, 1316, 999, 188, 6000};
            while (off < fixture->size() && g_ok)
            {
                rng              = rng * 1103515245U + 12345U;
                std::size_t want = pattern[pattern_idx++ % 8];
                if ((rng & 0x100U) != 0)
                {
                    want = 1 + static_cast<std::size_t>(rng % 5000); // random sizes too
                }
                want = std::min(want, fixture->size() - off);

                const double target  = static_cast<double>(off) / bytes_per_sec;
                const auto   elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - feed_t0);
                if (target > elapsed.count())
                {
                    std::this_thread::sleep_for(std::chrono::duration<double>(target - elapsed.count()));
                }

                if (!challenger_done && off >= fixture->size() / 2)
                {
                    challenger_done = true;
                    websrt::Publisher              challenger;
                    const websrt::PublisherConfig& ccfg = cfg;
                    const auto                     cst  = challenger.Start(ccfg);
                    Check([&] { return cst == websrt::kPublishRejected; },
                          "mid-stream challenger must be kPublishRejected (got " + std::to_string(cst) + ")");
                    Check([&] { return Contains(challenger.LastError(), "403"); },
                          "challenger LastError must name HTTP 403 (got \"" + challenger.LastError() + "\")");
                    challenger.Stop();
                    std::printf("live gate: first-wins challenger rejected (403) at byte %zu\n", off);
                }

                const char* p    = reinterpret_cast<const char*>(fixture->data()) + off;
                std::size_t left = want;
                while (left > 0)
                {
                    const int n = pub->Write(p, left, 2000);
                    if (n < 0)
                    {
                        Fail("pub.Write failed at byte " + std::to_string(off) + ": " + pub->LastError());
                        break;
                    }
                    p += n;
                    left -= static_cast<std::size_t>(n);
                }
                if (!g_ok)
                {
                    break;
                }
                off += want;
            }

            // (e) Publisher stats while still connected (pre-End): sane
            // send counters and a survived retransmit story.
            websrt::PublisherStats pstats;
            if (pub->Stats(&pstats) == websrt::kStatsOk)
            {
                const auto expect_pkts = static_cast<std::int64_t>((fixture->size() + 1127) / 1128);
                Check([&] { return pstats.packets_sent >= expect_pkts; },
                      "packets_sent >= ceiling(size/1128) (got " + std::to_string(pstats.packets_sent) + " vs " +
                          std::to_string(expect_pkts) + ")");
                Check([&] { return pstats.bytes_sent >= fixture->size(); }, "bytes_sent counts the payload");
                Check([&] { return pstats.naks_received >= 0 && pstats.naks_received < 1000; },
                      "naks_received sane (got " + std::to_string(pstats.naks_received) + ")");
                Check([&] { return pstats.rtt_ms > 0.0 && pstats.rtt_ms < 1000.0; },
                      "rtt sane (got " + std::to_string(pstats.rtt_ms) + " ms)");
                std::printf("live gate: publisher stats: pkts=%lld bytes=%llu rtx=%lld naks=%lld rtt=%.3f\n",
                            static_cast<long long>(pstats.packets_sent),
                            static_cast<unsigned long long>(pstats.bytes_sent),
                            static_cast<long long>(pstats.packets_retransmitted),
                            static_cast<long long>(pstats.naks_received),
                            pstats.rtt_ms);
            }
            else
            {
                Fail("pub.Stats failed before End");
            }

            // Subscriber mid-stream stats: the stream is flowing.
            if (sub.Stats(&sub_stats_mid) == websrt::kStatsOk)
            {
                Check([&] { return sub_stats_mid.packets_received > 0; }, "subscriber received packets mid-stream");
            }

            // (f) Graceful end: flush + ACK drain + SRT shutdown through
            // the open WT; the gateway then ends the broadcast.
            Check([&] { return pub->End(10000); }, "pub.End must complete gracefully (" + pub->LastError() + ")");

            // (g) The subscriber drains to a clean EOF (the gateway
            // closes its session when the broadcast ring ends).
            std::vector<std::uint8_t> out;
            out.reserve(fixture->size() + 4096);
            char       buf[1128];
            const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            for (;;)
            {
                const int n = sub.Read(buf, sizeof buf, 1000);
                if (n > 0)
                {
                    out.insert(out.end(), buf, buf + n);
                    continue;
                }
                if (n == websrt::kReadEnd)
                {
                    break;
                }
                if (n == websrt::kReadTimeout)
                {
                    if (std::chrono::steady_clock::now() < drain_deadline)
                    {
                        continue;
                    }
                    Fail("subscriber drain timed out after 60 s");
                    break;
                }
                Fail("subscriber Read ended with " + std::to_string(n) + ": " + sub.LastError());
                break;
            }
            dumped = std::move(out);

            // (h) Subscriber final stats were unreadable post-EOF
            // (kStatsNotConnected is the documented behavior); the
            // mid-stream snapshot stands as the survival evidence.
            sub.Stop();
            attached = true; // the dance ran once; retry only covered Start
        }
        if (!attached && g_ok)
        {
            Fail("subscriber never attached");
        }
    }

    // (i) THE assertion: byte-for-byte against the fixture.
    if (g_ok)
    {
        const std::size_t dump_n  = dumped.has_value() ? dumped->size() : 0;
        const std::size_t fix_n   = fixture->size();
        std::size_t       diff_at = dump_n;
        const std::size_t common  = std::min(dump_n, fix_n);
        for (std::size_t i = 0; i < common; ++i)
        {
            if ((*dumped)[i] != (*fixture)[i])
            {
                diff_at = i;
                break;
            }
        }
        Check([&] { return dump_n == fix_n && diff_at == fix_n; },
              "dump must equal the fixture byte-for-byte (dumped " + std::to_string(dump_n) + " vs " +
                  std::to_string(fix_n) + " bytes; first difference at " +
                  (diff_at == dump_n && dump_n == common ? std::string("EOF-shortfall") : std::to_string(diff_at)) +
                  ")");
    }

    // (j) Occupancy released: after End the broadcaster dies and the
    // name frees, so a fresh Publisher can claim it (FFmpeg reuse).
    if (g_ok)
    {
        bool freed = false;
        for (int attempt = 0; attempt < 20 && !freed && g_ok; ++attempt)
        {
            if (attempt > 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            websrt::Publisher              re;
            const websrt::PublisherConfig& rcfg = cfg;
            const auto                     rst  = re.Start(rcfg);
            if (rst == websrt::kPublishOk)
            {
                freed = true;
                re.Stop();
            }
            else if (rst != websrt::kPublishRejected)
            {
                Fail("freed-name re-Start failed unexpectedly (got " + std::to_string(rst) + ": " + re.LastError() +
                     ")");
            }
        }
        Check([&] { return freed; }, "stream name freed after End");
    }

    pub->Stop();

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
    std::printf("live gate: complete in %lld ms; subscriber mid stats: pkts=%lld lost=%lld rtx=%lld dropped=%lld\n",
                static_cast<long long>(elapsed.count()),
                static_cast<long long>(sub_stats_mid.packets_received),
                static_cast<long long>(sub_stats_mid.packets_lost),
                static_cast<long long>(sub_stats_mid.packets_retransmitted),
                static_cast<long long>(sub_stats_mid.packets_dropped));
}

} // namespace

int main()
{
    TestConfigValidation();
    TestChunker();
    TestOfflineMisuse();
    TestLiveGate();

    if (!g_ok)
    {
        return 1;
    }
    std::printf("publisher_test: ALL CHECKS PASSED (%d)\n", g_checks);
    return 0;
}
