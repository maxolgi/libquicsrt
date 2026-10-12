// websrt-dump: subscribe to a WebSRT stream with libwebsrt and write the
// received MPEG-TS bytes to a file - the native counterpart of the browser
// player, and the P1 interop tool (scripts/interop.sh compares the dump
// byte-for-byte against the published fixture).
//
// Usage:
//   websrt-dump --host 127.0.0.1 --port 4433 --stream NAME --out FILE
//               [--duration SECONDS] [--wait-stream SECONDS]
//               (--cert-hash HEX | --cert-hash-file PATH | --pki)
//               [--stats-interval SECONDS] [--quiet]
//
// --cert-hash-file parses the gateway's runtime cert-hash.js (the value
// assigned to CERT_HASH; `null` means PKI mode). --wait-stream retries
// Start every 500 ms while the gateway answers 404 (no publisher yet), so
// a subscriber can attach before the ingest starts publishing.
//
// Exit codes: 0 clean end (EOF, --duration reached, or SIGINT/SIGTERM),
// 3 still rejected when the --wait-stream window expired, 4 Start failure
// or mid-stream error, 5 usage.
//
// Stats go to STDERR, one key=value line each: periodic lines while
// streaming (interval 0 disables them) plus a final `done` line. The
// payloads written to --out are pure TS bytes; the SRT byte counters in
// the stats lines include the per-packet SRT/UDP/IP header overhead.

#include <csignal>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "websrt/subscriber.hpp"

namespace
{

// Exit codes (script-facing contract).
constexpr int kExitOk     = 0;
constexpr int kExitWait   = 3;
constexpr int kExitStream = 4;
constexpr int kExitUsage  = 5;

constexpr int kRetryDelayMs = 500; // --wait-stream retry cadence
constexpr int kReadChunksMs = 250; // Read timeout; bounds flag latency

std::atomic<bool> g_stop{false};

void OnSignal(int /*signum*/)
{
    g_stop.store(true);
}

void Usage(FILE* out)
{
    std::fprintf(out,
                 "Usage: websrt-dump --host HOST --stream NAME --out FILE [options]\n"
                 "\n"
                 "Options:\n"
                 "  --host HOST             gateway IPv4 address (required)\n"
                 "  --port PORT             gateway UDP port (default 443)\n"
                 "  --stream NAME           stream name to subscribe to (required)\n"
                 "  --out FILE              output file for the TS bytes (required)\n"
                 "  --duration SECONDS      stop after SECONDS of streaming (0 = until EOF)\n"
                 "  --wait-stream SECONDS   retry Start while rejected, until a publisher\n"
                 "                          appears (default 0 = one attempt)\n"
                 "  --cert-hash HEX         SHA-256 of the gateway cert DER, 64 hex chars\n"
                 "  --cert-hash-file PATH   parse the gateway's cert-hash.js\n"
                 "  --pki                   normal PKI validation instead of pinning\n"
                 "  --stats-interval SECS   stats line cadence on stderr (default 1, 0 = off)\n"
                 "  --quiet                 no periodic stats or informational lines\n");
}

void FailUsage(const std::string& what)
{
    std::fprintf(stderr, "websrt-dump: %s\n", what.c_str());
    Usage(stderr);
    std::exit(kExitUsage);
}

bool ParseDouble(const char* text, double* out)
{
    char* end = nullptr;
    *out      = std::strtod(text, &end);
    return end != text && *end == '\0';
}

bool ParseLong(const char* text, long* out)
{
    char* end = nullptr;
    *out      = std::strtol(text, &end, 10);
    return end != text && *end == '\0';
}

bool IsHex(char c)
{
    return std::strchr("0123456789abcdefABCDEF", c) != nullptr;
}

double SecondsSince(std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

// The gateway writes `window.CERT_HASH = "<64 hex>";` (self-signed mode)
// or `window.CERT_HASH = null;` (PKI mode). Returns false on anything
// else - strict, so a broken file fails the run instead of the stream.
bool ParseCertHashJs(const std::string& path, std::string* hex_out, bool* pki_out)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
    {
        return false;
    }
    std::string js;
    char        buf[4096];
    for (;;)
    {
        const size_t n = std::fread(buf, 1, sizeof buf, f);
        js.append(buf, n);
        if (n < sizeof buf)
        {
            break; // short read: EOF or error, either way stop
        }
    }
    std::fclose(f);

    const size_t key = js.find("CERT_HASH");
    if (key == std::string::npos)
    {
        return false;
    }
    size_t i = js.find_first_not_of(" \t\r\n", key + std::strlen("CERT_HASH"));
    if (i == std::string::npos || js[i] != '=')
    {
        return false;
    }
    i = js.find_first_not_of(" \t\r\n", i + 1);
    if (i == std::string::npos)
    {
        return false;
    }
    if (js.compare(i, 4, "null") == 0)
    {
        *pki_out = true;
        return true;
    }
    if (js[i] != '"' || js.size() < i + 1 + 64 || js[i + 1 + 64] != '"')
    {
        return false;
    }
    const std::string hex = js.substr(i + 1, 64);
    for (const char c : hex)
    {
        if (!IsHex(c))
        {
            return false;
        }
    }
    *hex_out = hex;
    return true;
}

// One key=value stats line on stderr. pkts/bytes follow SRT's convention
// (bytes include the 44 B/pkt header overhead); wrote_* are the pure TS
// payload bytes this process wrote to --out.
void PrintStats(
    const char* tag, double seconds, long long wrote_bytes, long long wrote_pkts, const websrt::SubscriberStats& s)
{
    std::fprintf(stderr,
                 "websrt-dump: %s t=%.1fs wrote_bytes=%lld wrote_pkts=%lld pkts=%lld bytes=%llu"
                 " lost=%lld rtx=%lld dropped=%lld belated=%lld rtt=%.1fms rate=%.2fmbps\n",
                 tag,
                 seconds,
                 wrote_bytes,
                 wrote_pkts,
                 static_cast<long long>(s.packets_received),
                 static_cast<unsigned long long>(s.bytes_received),
                 static_cast<long long>(s.packets_lost),
                 static_cast<long long>(s.packets_retransmitted),
                 static_cast<long long>(s.packets_dropped),
                 static_cast<long long>(s.packets_belated),
                 s.rtt_ms,
                 s.recv_rate_mbps);
}

} // namespace

int main(int argc, char** argv)
{
    std::string host;
    int         port = 443;
    std::string stream;
    std::string out_path;
    double      duration_s    = 0.0;
    double      wait_stream_s = 0.0;
    std::string cert_hex;
    bool        pki        = false;
    bool        have_cert  = false;
    double      stats_iv_s = 1.0;
    bool        quiet      = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg  = argv[i];
        auto              next = [&]() -> const char*
        {
            if (i + 1 >= argc)
            {
                FailUsage(arg + " requires a value");
            }
            return argv[++i];
        };
        if (arg == "--host")
        {
            host = next();
        }
        else if (arg == "--port")
        {
            long v = 0;
            if (!ParseLong(next(), &v) || v < 1 || v > 65535)
            {
                FailUsage("--port must be 1..65535");
            }
            port = static_cast<int>(v);
        }
        else if (arg == "--stream")
        {
            stream = next();
        }
        else if (arg == "--out")
        {
            out_path = next();
        }
        else if (arg == "--duration")
        {
            if (!ParseDouble(next(), &duration_s) || duration_s < 0)
            {
                FailUsage("--duration must be >= 0");
            }
        }
        else if (arg == "--wait-stream")
        {
            if (!ParseDouble(next(), &wait_stream_s) || wait_stream_s < 0)
            {
                FailUsage("--wait-stream must be >= 0");
            }
        }
        else if (arg == "--cert-hash")
        {
            cert_hex = next();
            if (cert_hex.size() != 64)
            {
                FailUsage("--cert-hash must be 64 hex characters");
            }
            for (const char c : cert_hex)
            {
                if (!IsHex(c))
                {
                    FailUsage("--cert-hash must be 64 hex characters");
                }
            }
            have_cert = true;
        }
        else if (arg == "--cert-hash-file")
        {
            if (!ParseCertHashJs(next(), &cert_hex, &pki))
            {
                FailUsage("--cert-hash-file: no parseable CERT_HASH value");
            }
            have_cert = true;
        }
        else if (arg == "--pki")
        {
            pki       = true;
            have_cert = true;
        }
        else if (arg == "--stats-interval")
        {
            if (!ParseDouble(next(), &stats_iv_s) || stats_iv_s < 0)
            {
                FailUsage("--stats-interval must be >= 0");
            }
        }
        else if (arg == "--quiet")
        {
            quiet = true;
        }
        else if (arg == "--help" || arg == "-h")
        {
            Usage(stdout);
            return kExitOk;
        }
        else
        {
            FailUsage("unknown option: " + arg);
        }
    }

    if (host.empty() || stream.empty() || out_path.empty())
    {
        FailUsage("--host, --stream and --out are required");
    }
    if (!have_cert)
    {
        FailUsage("one of --cert-hash, --cert-hash-file or --pki is required");
    }
    if (pki && !cert_hex.empty())
    {
        FailUsage("--pki conflicts with a pinned cert hash");
    }

    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_handler = OnSignal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    websrt::SubscriberConfig cfg;
    cfg.host            = host;
    cfg.port            = static_cast<std::uint16_t>(port);
    cfg.stream_name     = stream;
    cfg.cert_sha256_hex = pki ? std::string() : cert_hex;

    // Start with --wait-stream retry. The gateway answers HTTP 404 until a
    // publisher registers the stream; each attempt needs a fresh Subscriber
    // (single-shot instances). kStartTimedOut is retried too - the local
    // oracle's accept loop stalls transiently - but only within the window.
    const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(wait_stream_s);
    std::unique_ptr<websrt::Subscriber> sub;
    for (;;)
    {
        sub                              = std::make_unique<websrt::Subscriber>();
        const websrt::StartStatus status = sub->Start(cfg);
        if (status == websrt::kStartOk)
        {
            break;
        }
        const std::string error = sub->LastError();
        if (status == websrt::kStartRejected || status == websrt::kStartTimedOut)
        {
            if (std::chrono::steady_clock::now() >= wait_deadline)
            {
                std::fprintf(stderr, "websrt-dump: giving up on stream '%s': %s\n", stream.c_str(), error.c_str());
                return kExitWait;
            }
            if (!quiet)
            {
                std::fprintf(stderr, "websrt-dump: stream '%s' not available yet, retrying\n", stream.c_str());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kRetryDelayMs));
            continue;
        }
        std::fprintf(stderr, "websrt-dump: Start failed (%d): %s\n", static_cast<int>(status), error.c_str());
        return kExitStream;
    }

    FILE* out = std::fopen(out_path.c_str(), "wb");
    if (out == nullptr)
    {
        std::fprintf(stderr, "websrt-dump: cannot open '%s' for writing\n", out_path.c_str());
        return kExitStream;
    }
    if (!quiet)
    {
        std::fprintf(stderr,
                     "websrt-dump: subscribed to '%s' at %s:%d (%s)\n",
                     stream.c_str(),
                     host.c_str(),
                     port,
                     pki ? "pki" : "pinned");
    }

    const auto              started     = std::chrono::steady_clock::now();
    auto                    last_stats  = started;
    long long               wrote_bytes = 0;
    long long               wrote_pkts  = 0;
    websrt::SubscriberStats last{};
    const char*             reason    = "eof";
    int                     exit_code = kExitOk;
    std::string             error;

    // One periodic stats line per interval, sampled on wall clock from
    // both data and timeout Read results: data may flow for minutes
    // without a single timeout, and the totals are unreadable after the
    // connection ends, so the final line needs a live snapshot kept.
    auto maybe_stats = [&]
    {
        if (quiet || stats_iv_s <= 0 ||
            std::chrono::duration<double>(std::chrono::steady_clock::now() - last_stats).count() < stats_iv_s)
        {
            return;
        }
        websrt::SubscriberStats s;
        if (sub->Stats(&s) == websrt::kStatsOk)
        {
            last = s;
        }
        PrintStats("stats", SecondsSince(started), wrote_bytes, wrote_pkts, last);
        last_stats = std::chrono::steady_clock::now();
    };

    std::vector<char> buf(cfg.payload_size);
    for (;;)
    {
        if (g_stop.load())
        {
            reason = "signal";
            break;
        }
        if (duration_s > 0 && SecondsSince(started) >= duration_s)
        {
            reason = "duration";
            break;
        }
        const int n = sub->Read(buf.data(), static_cast<int>(buf.size()), kReadChunksMs);
        if (n > 0)
        {
            if (std::fwrite(buf.data(), 1, static_cast<size_t>(n), out) != static_cast<size_t>(n))
            {
                error     = "write to '" + out_path + "' failed";
                reason    = "error";
                exit_code = kExitStream;
                break;
            }
            wrote_bytes += n;
            ++wrote_pkts;
            maybe_stats();
            continue;
        }
        if (n == websrt::kReadTimeout)
        {
            maybe_stats();
            continue;
        }
        if (n == websrt::kReadEnd)
        {
            break;
        }
        error     = sub->LastError();
        reason    = "error";
        exit_code = kExitStream;
        break;
    }

    // Final snapshot: the totals are unreadable once the connection ended,
    // so keep the last live one when Stats declines.
    {
        websrt::SubscriberStats s;
        if (sub->Stats(&s) == websrt::kStatsOk)
        {
            last = s;
        }
    }
    sub->Stop();
    std::fflush(out);
    if (std::fclose(out) != 0 && exit_code == kExitOk)
    {
        error     = "closing '" + out_path + "' failed";
        reason    = "error";
        exit_code = kExitStream;
    }

    const double total_s = SecondsSince(started);
    PrintStats("done", total_s, wrote_bytes, wrote_pkts, last);
    std::fprintf(
        stderr, "websrt-dump: %s after %.1fs, %lld bytes in %lld messages\n", reason, total_s, wrote_bytes, wrote_pkts);
    if (!error.empty())
    {
        std::fprintf(stderr, "websrt-dump: error: %s\n", error.c_str());
    }
    return exit_code;
}
