// wt_echo: P1 subscriber-primitive validation (libwebsrt.md, Risks #1),
// now on the libwebsrt foundation (EventLoop + WTClient). A WebTransport
// session is opened to the live Rust gateway, the gateway's SRT Conclusion
// handshake is received as WT datagrams, and garbage WT datagrams are sent
// back. This validates the primitive the whole subscriber and publisher
// stack rests on.
//
// The oracle is the local supervisord `websrt` service (UDP 127.0.0.1:4433)
// plus its runtime cert-hash.js. When either is absent the test exits with
// code 77 (registered as ctest's SKIP_RETURN_CODE) so `./build.sh test` stays
// green on machines without the Rust world. With the oracle up, every check
// below must genuinely pass.
//
// All session state is driven on the event-loop thread (listener callbacks
// and timers); the main thread only waits for completion, stops the loop,
// and evaluates the quiesced evidence.

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/event_loop.h"
#include "wt/client.h"

namespace
{

constexpr int kSkipExit = 77;

// Loopback oracle endpoints. The gateway host:port and cert-hash.js path can
// be overridden for remote-gateway experiments.
constexpr char kDefaultGateway[] = "127.0.0.1:4433";

constexpr std::chrono::seconds      kHandshakeDeadline{5}; // no UDP life by then -> SKIP
constexpr std::chrono::seconds      kObserveWindow{3};     // datagram round-trip observation
constexpr std::chrono::milliseconds kSendPeriodMs{250};
constexpr std::chrono::seconds      kTotalDeadline{15};  // hard stop for the whole run
constexpr std::chrono::seconds      kMainWaitBudget{25}; // main-thread backstop (ctest: 30)
constexpr char                      kGarbage[] = "wt-echo non-SRT garbage";

void Log(const char* fmt, ...)
{
    std::fputs("wt_echo: ", stdout);
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fputc('\n', stdout);
}

// cert-hash.js: `window.CERT_HASH = "<64 hex chars>";` (self-signed mode).
// Returns nullopt when the file is missing, CERT_HASH is null (mkcert mode),
// or the value does not parse as a SHA-256 hex digest.
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

struct Harness final : public websrt::WTClientListener
{
    websrt::EventLoop loop;
    websrt::WTClient  client{loop, *this};

    // Evidence: written on the loop thread, read by main after loop.Stop().
    bool                      ready_                        = false;
    bool                      session_closed_               = false;
    bool                      stream_terminal_              = false;
    bool                      window_started_               = false;
    bool                      open_at_window_end_           = false;
    bool                      skip_no_udp_                  = false;
    bool                      first_dgram_is_srt_handshake_ = false;
    int                       dg_in_                        = 0;
    std::size_t               dg_in_min_                    = SIZE_MAX;
    std::size_t               dg_in_max_                    = 0;
    int                       hist_lt64_ = 0, hist_lt256_ = 0, hist_lt1200_ = 0, hist_lt1400_ = 0, hist_ge1400_ = 0;
    std::vector<std::uint8_t> first_dgram_;
    std::string               close_reason_;
    std::string               fail_reason_;

    websrt::EventLoop::TimerId send_timer_     = websrt::EventLoop::kInvalidTimerId;
    websrt::EventLoop::TimerId window_timer_   = websrt::EventLoop::kInvalidTimerId;
    websrt::EventLoop::TimerId deadline_timer_ = websrt::EventLoop::kInvalidTimerId;
    websrt::EventLoop::TimerId hsk_timer_      = websrt::EventLoop::kInvalidTimerId;

    // Completion: set on the loop thread, awaited by main.
    std::mutex              mtx_;
    std::condition_variable cv_;
    bool                    done_ = false;

    void Fail(const std::string& why)
    {
        if (fail_reason_.empty())
        {
            fail_reason_ = why;
        }
    }

    void Finish()
    {
        {
            const std::lock_guard<std::mutex> lock(mtx_);
            done_ = true;
        }
        cv_.notify_all();
    }

    bool Done()
    {
        const std::lock_guard<std::mutex> lock(mtx_);
        return done_;
    }

    // ---- WTClientListener (event-loop thread) ----

    void OnSessionReady() override
    {
        ready_ = true;
        Log("session ready (extended CONNECT accepted)");
        send_timer_ = loop.SetTimer(kSendPeriodMs, true, [this] { SendGarbage(); });
    }

    void OnSessionClosed(const std::string& reason) override
    {
        session_closed_ = true;
        close_reason_   = reason;
        Log("session closed: %s", reason.c_str());
        Finish();
    }

    void OnDatagram(const std::uint8_t* data, std::size_t size) override
    {
        ++dg_in_;
        dg_in_min_ = std::min(dg_in_min_, size);
        dg_in_max_ = std::max(dg_in_max_, size);
        if (size < 64)
        {
            ++hist_lt64_;
        }
        else if (size < 256)
        {
            ++hist_lt256_;
        }
        else if (size < 1200)
        {
            ++hist_lt1200_;
        }
        else if (size < 1400)
        {
            ++hist_lt1400_;
        }
        else
        {
            ++hist_ge1400_;
        }
        if (first_dgram_.empty())
        {
            first_dgram_.assign(data, data + size);
            if (!window_started_)
            {
                window_started_ = true;
                window_timer_   = loop.SetTimer(kObserveWindow, false, [this] { FinishWindow(); });
            }
        }
    }

    void OnStreamReadable() override
    {
        // Terminal session-stream event: CLOSE capsule, FIN, or stream close.
        stream_terminal_ = true;
    }

    // ---- timer bodies (event-loop thread) ----

    void SendGarbage()
    {
        if (!client.SendDatagram(kGarbage, sizeof kGarbage - 1) && !session_closed_)
        {
            Log("SendDatagram rejected (session closing?)");
        }
    }

    void FinishWindow()
    {
        // Snapshot before our own clean teardown, which must not count
        // against "stayed open".
        open_at_window_end_ = !stream_terminal_ && !session_closed_;
        Log("observation window complete (session open: %s); closing cleanly", open_at_window_end_ ? "yes" : "no");
        if (send_timer_ != websrt::EventLoop::kInvalidTimerId)
        {
            loop.CancelTimer(send_timer_);
        }
        client.Close();
    }

    void OnHandshakeDeadline()
    {
        if (ready_ || Done() || client.UdpPacketsReceived() > 0)
        {
            return;
        }
        Log("no UDP life from the gateway within %lld s: nothing is listening",
            static_cast<long long>(kHandshakeDeadline.count()));
        skip_no_udp_ = true;
        Finish();
    }

    void OnHardDeadline()
    {
        if (Done())
        {
            return;
        }
        Fail("hard deadline reached");
        client.Close(); // graceful drain; OnSessionClosed finishes the run
    }

    // ---- verdict (main thread, after loop.Stop()) ----

    void LogFirstDatagramAnatomy()
    {
        if (first_dgram_.empty())
        {
            return;
        }
        const std::uint8_t* p = first_dgram_.data();
        const std::size_t   n = first_dgram_.size();
        Log("first datagram: %u bytes, hex prefix: %s",
            static_cast<unsigned>(n),
            HexStr(p, std::min<std::size_t>(n, 24)).c_str());
        // SRT control packet (big-endian): 16-byte header, then HandshakeControlInfo
        // per srt-protocol's HandshakeControlInfo::serialize.
        if (n >= 40)
        {
            const bool     control     = (p[0] & 0x80) != 0;
            const unsigned packet_type = ((p[0] & 0x7f) << 8) | p[1];
            const unsigned version     = (p[16] << 24) | (p[17] << 16) | (p[18] << 8) | p[19];
            const unsigned type_flags  = (p[20] << 24) | (p[21] << 16) | (p[22] << 8) | p[23];
            const unsigned shake_type  = (p[36] << 24) | (p[37] << 16) | (p[38] << 8) | p[39];
            const char*    shake_name  = "?";
            if (shake_type == 0xffffffff)
            {
                shake_name = "CONCLUSION";
            }
            else if (shake_type == 0)
            {
                shake_name = "INDUCTION";
            }
            Log("first datagram anatomy: control=%u packet_type=%u (0=handshake) version=%u type_flags=0x%08x "
                "shake_type=0x%08x (%s)",
                control ? 1 : 0,
                packet_type,
                version,
                type_flags,
                shake_type,
                shake_name);
            first_dgram_is_srt_handshake_ = control && packet_type == 0 && version == 5 && shake_type == 0xffffffff;
        }
    }

    int Evaluate()
    {
        Log("-- evidence ------------------------------------------------------");
        Log("session established         : %s", ready_ ? "yes" : "NO");
        Log("HTTP status                 : %d", client.HttpStatus());
        Log("close reason                : %s", close_reason_.empty() ? "(none)" : close_reason_.c_str());
        Log("smoothed RTT                : %llu us", static_cast<unsigned long long>(client.SmoothedRtt().count()));
        Log("WT datagrams in             : %d (sizes min..max %u..%u; <64:%d <256:%d <1200:%d <1400:%d >=1400:%d)",
            dg_in_,
            static_cast<unsigned>(dg_in_min_),
            static_cast<unsigned>(dg_in_max_),
            hist_lt64_,
            hist_lt256_,
            hist_lt1200_,
            hist_lt1400_,
            hist_ge1400_);
        LogFirstDatagramAnatomy();
        Log("WT datagrams out written    : %d (dropped %d)", client.DatagramsWritten(), client.DatagramsDropped());
        Log("session end state           : stream_terminal=%d session_closed=%d",
            stream_terminal_ ? 1 : 0,
            session_closed_ ? 1 : 0);
        Log("UDP packets received        : %d", client.UdpPacketsReceived());
        Log("------------------------------------------------------------------");

        bool ok      = true;
        auto require = [&ok](bool cond, const char* what)
        {
            Log("%-42s: %s", what, cond ? "PASS" : "FAIL");
            ok = ok && cond;
        };
        require(ready_, "handshake + cert pin + CONNECT");
        require(client.HttpStatus() == 200, "session accepted (HTTP 200)");
        require(dg_in_ >= 1, "received >= 1 WT datagram");
        require(first_dgram_is_srt_handshake_, "first datagram is SRT HSv5 Conclusion");
        require(client.DatagramsWritten() >= 1, "sent >= 1 WT datagram (datagram-write path)");
        require(open_at_window_end_, "session stayed open (observation window)");
        if (!fail_reason_.empty())
        {
            Log("FAIL: %s", fail_reason_.c_str());
            ok = false;
        }
        if (ok)
        {
            Log("wt_echo: ALL ASSERTIONS PASSED");
        }
        return ok ? 0 : 1;
    }

    static std::string HexStr(const std::uint8_t* p, std::size_t n)
    {
        static const char kHex[] = "0123456789abcdef";
        std::string       s;
        s.reserve(n * 2);
        for (std::size_t i = 0; i < n; ++i)
        {
            s += kHex[p[i] >> 4];
            s += kHex[p[i] & 0xf];
        }
        return s;
    }
};

} // namespace

int main()
{
    if (const char* level = std::getenv("WT_ECHO_LSQUIC_LOG"))
    {
        websrt::SetLsquicLogLevel(level);
    }

    const auto hash_hex = LoadCertHashHex();
    if (!hash_hex)
    {
        Log("SKIP: cert-hash.js unavailable or not in self-signed mode");
        return kSkipExit;
    }
    Log("pinned cert hash: %s", hash_hex->c_str());

    const char*       gateway_env = std::getenv("WEBSRT_WT_GATEWAY");
    const std::string gateway     = gateway_env != nullptr && *gateway_env != '\0' ? gateway_env : kDefaultGateway;
    const std::size_t colon       = gateway.rfind(':');
    if (colon == std::string::npos)
    {
        Log("SKIP: gateway must be host:port, got \"%s\"", gateway.c_str());
        return kSkipExit;
    }
    websrt::WTClient::Target target;
    target.host          = gateway.substr(0, colon);
    target.port          = static_cast<std::uint16_t>(std::atoi(gateway.c_str() + colon + 1));
    target.path          = "/wt?publish=wt_echo_" + std::to_string(static_cast<long>(::getpid()));
    target.cert_hash_hex = *hash_hex;

    Harness harness;
    // Deadline timers are armed while the loop is not running yet (allowed:
    // registration is loop-thread-only or pre-Start).
    harness.deadline_timer_ = harness.loop.SetTimer(kTotalDeadline, false, [&harness] { harness.OnHardDeadline(); });
    harness.hsk_timer_ =
        harness.loop.SetTimer(kHandshakeDeadline, false, [&harness] { harness.OnHandshakeDeadline(); });

    harness.loop.Start();
    if (!harness.client.Connect(target))
    {
        Log("SKIP: target rejected (gateway \"%s\")", gateway.c_str());
        harness.loop.Stop();
        return kSkipExit;
    }
    Log("gateway %s, path %s", gateway.c_str(), target.path.c_str());

    {
        std::unique_lock<std::mutex> lock(harness.mtx_);
        harness.cv_.wait_for(lock, kMainWaitBudget, [&harness] { return harness.done_; });
    }
    harness.loop.Stop();

    if (harness.skip_no_udp_)
    {
        return kSkipExit;
    }
    if (!harness.Done())
    {
        harness.Fail("main wait timed out (session never closed)");
    }
    return harness.Evaluate();
}
