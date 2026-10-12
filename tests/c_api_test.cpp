// c_api_test: the extern-C ABI over libwebsrt (websrt_c.h), the layer
// the FFmpeg websrt:// protocol links against.
//
// (1) Offline misuse: every function with NULL handles/args, write and
//     read before start, stats before start, bad start params, double
//     start, destroy without stop.
// (2) LIVE GATE against the local Rust gateway oracle (supervisord
//     `websrt` + runtime cert-hash.js + fixtures/test.ts): a C-API
//     publisher feeds the fixture once (paced ~10 s) while a C-API
//     subscriber (404 wait-retry) dumps it - byte-for-byte equality,
//     mid-stream first-wins challenger (403), stats strings non-empty,
//     name freed after end. Exits 77 (ctest SKIP_RETURN_CODE) when the
//     oracle is absent.

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "websrt/websrt_c.h"

namespace
{

constexpr int kSkipExit = 77;

constexpr char kDefaultGateway[] = "127.0.0.1:4433";

std::atomic<bool> g_ok{true};

void Fail(const std::string& what)
{
    g_ok = false;
    std::fprintf(stderr, "c_api_test: FAIL: %s\n", what.c_str());
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
// nullopt when the file is missing, the hash is null (mkcert mode), or
// the value does not parse - same contract as the other live tests.
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

// ---- (1) offline misuse ----

void TestOfflineMisuse()
{
    Check([&] { return std::strncmp(websrt_version(), "0.1", 3) == 0; }, "websrt_version must be 0.1.x");
    Check([&] { return websrt_publisher_last_error(nullptr) != nullptr; }, "publisher_last_error(NULL) is non-NULL");
    Check([&] { return websrt_subscriber_last_error(nullptr) != nullptr; }, "subscriber_last_error(NULL) is non-NULL");
    Check(
        [&] {
            return websrt_publisher_write(nullptr, "x", 1, 10) == WEBSRT_WRITE_INVALID &&
                   websrt_subscriber_read(nullptr, nullptr, 0, 10) == WEBSRT_READ_INVALID;
        },
        "NULL-handle pump calls are WEBSRT_*_INVALID");
    websrt_publisher_destroy(nullptr); // no-op, must not crash
    websrt_subscriber_destroy(nullptr);
    websrt_publisher_stop(nullptr);
    websrt_subscriber_stop(nullptr);

    websrt_publisher_t* pub = websrt_publisher_create();
    Check([&] { return pub != nullptr; }, "publisher_create");
    Check([&] { return websrt_publisher_start(pub, "127.0.0.1", 0, nullptr, nullptr, 0, 0) == WEBSRT_EINVAL; },
          "start with NULL stream is EINVAL");
    Check([&] { return websrt_publisher_write(pub, "x", 1, 10) == WEBSRT_WRITE_NOT_STARTED; },
          "write before start is NOT_STARTED");
    // The façade checks not-started before the args; NULL/zero-length
    // surface as INVALID only on a live session (checked in the live gate).
    Check(
        [&] {
            char stats[64];
            return websrt_publisher_stats(pub, stats, sizeof stats) == WEBSRT_ESTATE &&
                   websrt_publisher_stats(pub, nullptr, 100) == WEBSRT_EINVAL;
        },
        "stats before start is ESTATE; NULL buf is EINVAL");
    Check([&] { return websrt_publisher_end(pub, 10) != WEBSRT_OK; }, "end before start fails");
    Check(
        [&] {
            return websrt_publisher_start(pub, "127.0.0.1", 4433, "", "zz", 0, 1) == WEBSRT_ECONFIG &&
                   Contains(websrt_publisher_last_error(pub), "stream");
        },
        "invalid config (empty stream / bad hash) is ECONFIG");
    websrt_publisher_destroy(pub); // never started; clean destroy

    websrt_subscriber_t* sub = websrt_subscriber_create();
    Check([&] { return sub != nullptr; }, "subscriber_create");
    Check([&] { return websrt_subscriber_read(sub, nullptr, 1, 10) == WEBSRT_READ_NOT_STARTED; },
          "read NULL buf before start is NOT_STARTED (façade checks state first)");
    Check([&] { return websrt_subscriber_read(sub, nullptr, 0, 10) == WEBSRT_READ_NOT_STARTED; },
          "read before start is NOT_STARTED (NULL buf checked first)");
    char big[2048];
    Check([&] { return websrt_subscriber_read(sub, big, sizeof big, 10) == WEBSRT_READ_NOT_STARTED; },
          "read before start is NOT_STARTED");
    Check(
        [&] {
            char stats[64];
            return websrt_subscriber_stats(sub, stats, sizeof stats) == WEBSRT_ESTATE;
        },
        "subscriber stats before start is ESTATE");
    Check([&] { return std::strlen(websrt_subscriber_last_error(sub)) == 0; }, "subscriber last_error empty so far");
    websrt_subscriber_destroy(sub);
}

// ---- (2) live gate ----

std::optional<std::vector<std::uint8_t>> LoadFixture()
{
    const char* path = std::getenv("WEBSRT_FIXTURE_TS");
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

struct Gateway
{
    std::string host;
    int         port = 0;
    std::string hash;
};

std::optional<Gateway> GatewayTarget()
{
    const char*       gateway_env = std::getenv("WEBSRT_WT_GATEWAY");
    const std::string gateway     = gateway_env != nullptr && *gateway_env != '\0' ? gateway_env : kDefaultGateway;
    const std::size_t colon       = gateway.rfind(':');
    if (colon == std::string::npos)
    {
        return std::nullopt;
    }
    const auto hash = LoadCertHashHex();
    if (!hash)
    {
        return std::nullopt;
    }
    Gateway gw;
    gw.host = gateway.substr(0, colon);
    gw.port = std::atoi(gateway.c_str() + colon + 1);
    gw.hash = *hash;
    return gw;
}

void TestLiveGate()
{
    const auto fixture = LoadFixture();
    if (!fixture)
    {
        std::printf("c_api_test: SKIP: fixture unavailable\n");
        std::exit(kSkipExit);
    }
    const auto gw = GatewayTarget();
    if (!gw)
    {
        std::printf("c_api_test: SKIP: oracle target unavailable\n");
        std::exit(kSkipExit);
    }

    const std::string name = "websrt-cpp-capi-" + std::to_string(::getpid());

    // (a) publisher Start against the oracle (one deaf-oracle retry).
    websrt_publisher_t* pub  = nullptr;
    websrt_status_t     st   = WEBSRT_ETIMEOUT;
    for (int attempt = 0; attempt < 2 && st == WEBSRT_ETIMEOUT; ++attempt)
    {
        if (attempt > 0)
        {
            std::printf("live gate: gateway unresponsive, retrying in 3 s\n");
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
        websrt_publisher_destroy(pub);
        pub = websrt_publisher_create();
        st  = websrt_publisher_start(pub, gw->host.c_str(), gw->port, name.c_str(), gw->hash.c_str(), 0, 5000);
    }
    if (st == WEBSRT_ETIMEOUT)
    {
        websrt_publisher_destroy(pub);
        std::printf("c_api_test: SKIP: gateway oracle unresponsive\n");
        std::exit(kSkipExit);
    }
    Check([&] { return st == WEBSRT_OK; },
          "publisher start must succeed (got " + std::to_string(st) + ": " + websrt_publisher_last_error(pub) + ")");
    Check([&] { return websrt_publisher_start(pub, "127.0.0.1", 4433, "x", nullptr, 0, 1) == WEBSRT_ESTATE; },
          "double start is ESTATE");

    // (b) subscriber with 404 wait-retry (fresh single-shot handles),
    // then a 2 s pre-grace so the dump attaches before byte 0.
    websrt_subscriber_t* sub  = nullptr;
    bool                 have = false;
    for (int attempt = 0; attempt < 20 && !have; ++attempt)
    {
        if (attempt > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        websrt_subscriber_destroy(sub);
        sub  = websrt_subscriber_create();
        have = websrt_subscriber_start(sub, gw->host.c_str(), gw->port, name.c_str(), gw->hash.c_str(), 0, 5000) ==
               WEBSRT_OK;
    }
    Check([&] { return have; }, "subscriber must attach while the name is live");
    if (!have)
    {
        websrt_publisher_stop(pub);
        websrt_publisher_destroy(pub);
        return;
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // (c) live-session arg validation: the façade checks state first,
    // so INVALID only surfaces once a session is established.
    Check([&] { return websrt_publisher_write(pub, nullptr, 1, 10) == WEBSRT_WRITE_INVALID; },
          "live write NULL data is INVALID");
    Check([&] { return websrt_publisher_write(pub, "x", 0, 10) == WEBSRT_WRITE_INVALID; },
          "live write zero length is INVALID");
    Check([&] { return websrt_subscriber_read(sub, nullptr, 1, 10) == WEBSRT_READ_INVALID; },
          "live read NULL buf is INVALID");

    // (d) feed the fixture once, paced over ~10 s, varied chunk sizes;
    // midpoint first-wins challenger must take the typed rejection.
    const double           bytes_per_sec = static_cast<double>(fixture->size()) / 10.0;
    const auto             feed_t0       = std::chrono::steady_clock::now();
    std::size_t            off           = 0;
    std::size_t            pattern_idx   = 0;
    bool                   challenged    = false;
    const std::size_t      pattern[]     = {std::size_t{7} * 188, 4096, 2048, 700, 1316, 999, 188, 6000};
    std::vector<std::uint8_t> out;
    out.reserve(fixture->size() + 4096);
    while (off < fixture->size() && g_ok)
    {
        std::size_t want = pattern[pattern_idx++ % 8];
        want             = std::min(want, fixture->size() - off);

        const double target  = static_cast<double>(off) / bytes_per_sec;
        const auto   elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - feed_t0);
        if (target > elapsed.count())
        {
            std::this_thread::sleep_for(std::chrono::duration<double>(target - elapsed.count()));
        }

        if (!challenged && off >= fixture->size() / 2)
        {
            challenged          = true;
            websrt_publisher_t* ch = websrt_publisher_create();
            const websrt_status_t cst =
                websrt_publisher_start(ch, gw->host.c_str(), gw->port, name.c_str(), gw->hash.c_str(), 0, 5000);
            Check([&] { return cst == WEBSRT_EREJECTED; },
                  "challenger start is EREJECTED (got " + std::to_string(cst) + ")");
            Check([&] { return Contains(websrt_publisher_last_error(ch), "403"); },
                  "challenger last_error names HTTP 403");
            websrt_publisher_destroy(ch);
            std::printf("live gate: challenger rejected (403) at byte %zu\n", off);
        }

        const char* p    = reinterpret_cast<const char*>(fixture->data()) + off;
        std::size_t left = want;
        while (left > 0)
        {
            const int n = websrt_publisher_write(pub, p, left, 2000);
            if (n < 0)
            {
                Fail("publisher_write failed at byte " + std::to_string(off) + " (" + std::to_string(n) + "): " +
                     websrt_publisher_last_error(pub));
                break;
            }
            p += n;
            left -= static_cast<std::size_t>(n); // partial accept: resume
        }
        off += want;
    }

    // (e) stats strings while still connected.
    char stats[512];
    Check([&] { return websrt_publisher_stats(pub, stats, sizeof stats) == WEBSRT_OK && Contains(stats, "pkts_sent="); },
          "publisher stats string formats");
    std::printf("live gate: publisher stats: %s\n", stats);
    Check(
        [&] { return websrt_subscriber_stats(sub, stats, sizeof stats) == WEBSRT_OK && Contains(stats, "pkts_recv="); },
        "subscriber stats string formats");

    // (f) graceful end; the subscriber drains to a clean EOF.
    Check([&] { return websrt_publisher_end(pub, 10000) == WEBSRT_OK; },
          std::string("publisher end completes (") + websrt_publisher_last_error(pub) + ")");
    char       buf[1128];
    const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (;;)
    {
        const int n = websrt_subscriber_read(sub, buf, sizeof buf, 1000);
        if (n > 0)
        {
            out.insert(out.end(), buf, buf + n);
            continue;
        }
        if (n == WEBSRT_READ_END)
        {
            break;
        }
        if (n == WEBSRT_READ_TIMEOUT)
        {
            if (std::chrono::steady_clock::now() < drain_deadline)
            {
                continue;
            }
            Fail("subscriber drain timed out after 60 s");
            break;
        }
        if (n == WEBSRT_READ_CLOSED || n == WEBSRT_READ_ERROR)
        {
            Fail(std::string("subscriber read ended (") + std::to_string(n) + "): " + websrt_subscriber_last_error(sub));
            break;
        }
        Fail("unexpected subscriber read result " + std::to_string(n));
        break;
    }

    // (g) THE assertion: byte-for-byte against the fixture.
    const std::size_t dump_n = out.size();
    std::size_t       diff_at = dump_n == fixture->size() ? dump_n : std::min(dump_n, fixture->size());
    for (std::size_t i = 0; i < std::min(dump_n, fixture->size()); ++i)
    {
        if (out[i] != (*fixture)[i])
        {
            diff_at = i;
            break;
        }
    }
    Check([&] { return dump_n == fixture->size() && diff_at == fixture->size(); },
          "dump equals the fixture byte-for-byte (dumped " + std::to_string(dump_n) + " vs " +
              std::to_string(fixture->size()) + "; first diff at " + std::to_string(diff_at) + ")");

    websrt_subscriber_destroy(sub);
    websrt_publisher_destroy(pub);

    // (h) occupancy released: a fresh publisher can claim the name.
    bool freed = false;
    for (int attempt = 0; attempt < 20 && !freed && g_ok; ++attempt)
    {
        if (attempt > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        websrt_publisher_t* re = websrt_publisher_create();
        if (websrt_publisher_start(re, gw->host.c_str(), gw->port, name.c_str(), gw->hash.c_str(), 0, 5000) ==
            WEBSRT_OK)
        {
            freed = true;
        }
        websrt_publisher_destroy(re);
    }
    Check([&] { return freed; }, "stream name freed after end");

    // (i) cold 404: subscribing to a name nobody holds is EREJECTED
    // (the FFmpeg open path relies on this being a fast, typed failure).
    if (g_ok)
    {
        websrt_subscriber_t* miss = websrt_subscriber_create();
        const websrt_status_t mst =
            websrt_subscriber_start(miss, gw->host.c_str(), gw->port, "capi-no-such-stream-xyz", gw->hash.c_str(), 0,
                                    5000);
        Check([&] { return mst == WEBSRT_EREJECTED; },
              "subscribe to absent stream is EREJECTED (got " + std::to_string(mst) + ")");
        websrt_subscriber_destroy(miss);
    }
}

} // namespace

int main()
{
    TestOfflineMisuse();
    TestLiveGate();

    if (!g_ok)
    {
        return 1;
    }
    std::printf("c_api_test: ALL CHECKS PASSED (%d)\n", g_checks);
    return 0;
}
