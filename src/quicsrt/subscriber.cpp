// Subscriber façade (libquicsrt public API): owns the EventLoop, one
// WTClient, and the SubscriberSession, and exposes the blocking,
// exception-free surface declared in include/quicsrt/subscriber.hpp.
//
// Member construction is two-phase because the WTClient wants the session
// (its listener) by reference and the session wants the client (its
// transport): the session is constructed first, then the client, then
// AttachClient links them - all before Start, single-threaded.
//
// Destruction order is fixed by the WTClient contract: Stop() stops the
// event loop first (no further engine callbacks can fire), then the
// session is finalized (srt sockets closed, accept thread joined,
// transport handle released - only after that may the WTClient die,
// because srt_close sends shutdown through the send callback into it),
// and only then do the members destruct (session, client, loop - all
// quiescent by then). Teardown() is idempotent under an atomic
// exactly-once guard so the Start failure paths, a later Stop()/
// destructor, and a concurrent watchdog Stop cannot double-drop the srt
// library reference.

#include "quicsrt/subscriber.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <mutex>
#include <string>
#include <utility>

#include "core/event_loop.h"
#include "srt.h"
#include "srt_session/subscriber.h"
#include "quicsrt/resolve.h"
#include "quicsrt/start_status.h"
#include "wt/client.h"

namespace quicsrt
{

namespace
{

// Bound for how long Stop() waits for the graceful WT close to complete
// before stopping the loop underneath it. Shorter than the publisher's
// 5000: the subscriber only receives, so the drain is just the WT
// teardown handshake; there is no sender buffer left to flush.
constexpr int kStopDrainMs = 3000;

} // namespace

class Subscriber::Impl
{
public:
    Impl()
    {
        client_ = std::make_unique<WTClient>(loop_, *session_);
        session_->AttachClient(*client_);
    }

    ~Impl() { Stop(); }

    Impl(const Impl&)            = delete;
    Impl& operator=(const Impl&) = delete;

    StartStatus Start(const SubscriberConfig& config)
    {
        if (ever_started_)
        {
            return kStartAlreadyStarted;
        }

        const std::string config_error = config.validate();
        if (!config_error.empty())
        {
            SetError(config_error);
            return kStartInvalidConfig;
        }
        ever_started_ = true;

        SubscriberSessionParams params;
        params.payload_size     = config.payload_size;
        params.tsbpd_latency_ms = config.tsbpd_latency_ms;
        params.initial_rtt_ms   = config.initial_rtt_ms;
        session_->Configure(params);

        WTClient::Target target;
        target.host          = config.host;
        target.port          = config.port;
        target.path          = "/wt?stream=" + config.stream_name;
        target.cert_hash_hex = config.cert_sha256_hex;

        // Setup vs a concurrent Stop: a watchdog Stop can pass the
        // ever_started_ gate above and run its full Teardown while this
        // Start sits right here (CPU starvation makes the window real).
        // That Teardown's loop_.Stop() no-ops on a loop that does not
        // exist yet - so a loop started afterwards would outlive every
        // guard: nothing would ever stop it, and its posted
        // ConnectOnLoop would race the façade's destruction. Serialize
        // the setup with Teardown's stop side and re-check the guard
        // inside: either the loop comes up before any teardown (their
        // loop_.Stop() then stops and joins it) or a teardown already
        // ran and this Start skips the setup entirely.
        bool setup_done = false;
        {
            const std::lock_guard<std::mutex> lock(setup_mutex_);
            if (!torn_down_.load())
            {
                srt_startup(); // refcounted; paired with srt_cleanup() in Teardown()
                srt_up_ = true;
                loop_.Start();
                setup_done = true;
            }
        }
        if (!setup_done)
        {
            SetError("stopped before the event loop came up");
            start_failed_ = true;
            return kStartConnectFailed;
        }
        // Resolve on the calling thread (Start is blocking by design):
        // literals take CreateAddr's inet_pton fast path, names block on
        // the OS resolver's default timeout budget - outside
        // connect_timeout_ms, which governs the transport connect after
        // this point.
        std::string resolve_error;
        target.resolved_ip = ResolveHostToIpv4(config.host, &resolve_error);
        if (target.resolved_ip.empty())
        {
            SetError(resolve_error);
            start_failed_ = true;
            Teardown();
            return kStartInvalidTarget;
        }
        if (!client_->Connect(target))
        {
            // Connect rejects the target itself (bad dial address, zero
            // port) or finds the loop stopped under us - a concurrent
            // Stop's teardown already ran. Only the first is about the
            // target.
            const bool stopped_during_start = torn_down_.load();
            SetError(stopped_during_start ? "stopped during start"
                                          : "target rejected (port nonzero, path nonempty, cert hash malformed)");
            start_failed_ = true;
            Teardown();
            return stopped_during_start ? kStartConnectFailed : kStartInvalidTarget;
        }

        std::string why;
        if (session_->WaitEstablished(config.connect_timeout_ms, &why))
        {
            SetError(std::string());
            return kStartOk;
        }
        if (session_->IsClosed())
        {
            SetError(why.empty() ? "session closed during connect" : why);
            start_failed_ = true;
            Teardown();
            const int status = client_->HttpStatus();
            if (HttpStatusIsRejected(status))
            {
                return kStartRejected;
            }
            return kStartConnectFailed;
        }

        // connect_timeout_ms elapsed: tear the half-open session down.
        SetError("connect timeout after " + std::to_string(config.connect_timeout_ms) + " ms");
        start_failed_ = true;
        session_->RequestClose();
        session_->WaitClosed(kStopDrainMs);
        Teardown();
        return kStartTimedOut;
    }

    int Read(void* buf, std::size_t len, int timeout_ms)
    {
        if (!ever_started_)
        {
            return kReadNotStarted;
        }
        if (start_failed_)
        {
            return kReadClosed; // Start failed; LastError() has the reason
        }
        if (buf == nullptr || len == 0)
        {
            return kReadInvalidArg;
        }
        const int capped = static_cast<int>(std::min<std::size_t>(len, INT_MAX));
        const int result = session_->Read(static_cast<char*>(buf), capped, timeout_ms);
        if (result == kReadClosed || result == kReadError)
        {
            SetError(session_->close_reason());
        }
        return result;
    }

    StatsStatus Stats(SubscriberStats* out)
    {
        if (out == nullptr)
        {
            return kStatsNotConnected;
        }
        *out = SubscriberStats{};
        if (!ever_started_ || start_failed_)
        {
            return kStatsNotConnected;
        }

        SRT_TRACEBSTATS perf;
        if (!session_->Stats(&perf))
        {
            return kStatsNotConnected;
        }

        out->ms_elapsed            = perf.msTimeStamp;
        out->packets_received      = perf.pktRecvUniqueTotal;
        out->bytes_received        = perf.byteRecvUniqueTotal;
        out->packets_sent          = perf.pktSentUniqueTotal;
        out->bytes_sent            = perf.byteSentUniqueTotal;
        out->packets_lost          = perf.pktRcvLossTotal;
        out->packets_retransmitted = perf.pktRetransTotal;
        out->packets_dropped       = perf.pktRcvDropTotal;
        out->packets_belated       = perf.pktRcvBelated;
        out->acks_sent             = perf.pktSentACKTotal;
        out->acks_received         = perf.pktRecvACKTotal;
        out->naks_sent             = perf.pktSentNAKTotal;
        out->naks_received         = perf.pktRecvNAKTotal;

        out->rtt_ms            = perf.msRTT;
        out->recv_rate_mbps    = perf.mbpsRecvRate;
        out->send_rate_mbps    = perf.mbpsSendRate;
        out->bandwidth_mbps    = perf.mbpsBandwidth;
        out->flow_window       = perf.pktFlowWindow;
        out->congestion_window = perf.pktCongestionWindow;
        out->flight_size       = perf.pktFlightSize;
        out->packets_buffered  = perf.pktRcvBuf;
        out->bytes_buffered    = perf.byteRcvBuf;
        out->ms_buffered       = perf.msRcvBuf;
        out->ms_tsbpd_delay    = perf.msRcvTsbPdDelay;
        return kStatsOk;
    }

    // Stop is the one documented cross-thread entry (a watchdog racing
    // the controlling thread or the destructor): the exchange makes the
    // guard exactly-once, so exactly one caller runs the teardown
    // sequence and the losers return promptly.
    void Stop()
    {
        if (stopped_.exchange(true))
        {
            return; // another caller owns the teardown
        }
        if (!ever_started_ || torn_down_)
        {
            return;
        }
        session_->RequestClose();
        session_->WaitClosed(kStopDrainMs);
        Teardown();
    }

    [[nodiscard]] std::string LastError() const
    {
        const std::lock_guard<std::mutex> lock(error_mutex_);
        return last_error_;
    }

private:
    void SetError(const std::string& error)
    {
        const std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = error;
    }

    // Post-failure / post-stop common path: no more loop callbacks, srt
    // sockets closed, library reference dropped. Idempotent; the
    // exchange guard makes it exactly-once even when Stop and a Start
    // failure path race into it from two threads (a second srt_cleanup
    // would over-decrement the process-global srt refcount).
    void Teardown()
    {
        if (torn_down_.exchange(true))
        {
            return;
        }
        // Under setup_mutex_ an in-flight Start is either past its setup
        // (the loop exists and is joined here; srt_up_ says whose
        // srt_startup() this cleanup pairs) or has not entered it yet
        // (it then sees torn_down_ and skips the setup - no loop
        // resurrection, no unpaired cleanup).
        const std::lock_guard<std::mutex> lock(setup_mutex_);
        loop_.Stop();
        session_->Finalize();
        if (srt_up_)
        {
            srt_cleanup();
        }
    }

    // Declaration order = destruction order REVERSED. Destruction must
    // run: session_ first (its srt teardown may still fire the send
    // callback into client_), then client_ (its Teardown needs the loop's
    // epoll), then loop_ last. Hence: loop_ declared first, session_ last.
    EventLoop                          loop_;
    std::unique_ptr<WTClient>          client_;
    std::unique_ptr<SubscriberSession> session_ = std::make_unique<SubscriberSession>(SubscriberSessionParams{});

    // Lifecycle flags. stopped_/torn_down_ are atomic test-and-set
    // guards (see Stop/Teardown); ever_started_ is atomic because Stop -
    // the cross-thread entry - reads it while Start may still be setting
    // it. start_failed_ is only touched by the controlling/Read thread.
    std::atomic<bool> ever_started_ = false;
    bool              start_failed_ = false;
    std::atomic<bool> stopped_      = false;
    std::atomic<bool> torn_down_    = false;

    // Serializes Start's setup (srt_startup + loop_.Start) against
    // Teardown's stop side; srt_up_ records whether this façade's own
    // srt_startup() ran, so the cleanup pairs exactly. Both only under
    // setup_mutex_.
    std::mutex setup_mutex_;
    bool       srt_up_ = false;

    mutable std::mutex error_mutex_;
    std::string        last_error_;
};

Subscriber::Subscriber()
    : impl_(std::make_unique<Impl>())
{
}

Subscriber::~Subscriber() = default;

StartStatus Subscriber::Start(const SubscriberConfig& config)
{
    return impl_->Start(config);
}

int Subscriber::Read(void* buf, std::size_t len, int timeout_ms)
{
    return impl_->Read(buf, len, timeout_ms);
}

StatsStatus Subscriber::Stats(SubscriberStats* out)
{
    return impl_->Stats(out);
}

void Subscriber::Stop()
{
    impl_->Stop();
}

std::string Subscriber::LastError() const
{
    return impl_->LastError();
}

} // namespace quicsrt
