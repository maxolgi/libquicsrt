// Subscriber façade (libwebsrt public API): owns the EventLoop, one
// WTClient, and the SubscriberSession, and exposes the blocking,
// exception-free surface declared in include/websrt/subscriber.hpp.
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
// quiescent by then). Teardown() is idempotent so the Start failure
// paths and a later Stop()/destructor cannot double-drop the srt
// library reference.

#include "websrt/subscriber.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <mutex>
#include <string>
#include <utility>

#include "core/event_loop.h"
#include "srt.h"
#include "srt_session/subscriber.h"
#include "websrt/resolve.h"
#include "wt/client.h"

namespace websrt
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

        srt_startup(); // refcounted; paired with srt_cleanup() in Teardown()
        loop_.Start();
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
            SetError("target rejected (port nonzero, path nonempty, cert hash malformed)");
            start_failed_ = true;
            Teardown();
            return kStartInvalidTarget;
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
            if (status != -1 && status != 200)
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

    void Stop()
    {
        if (stopped_)
        {
            return;
        }
        stopped_ = true;
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
    // sockets closed, library reference dropped. Idempotent.
    void Teardown()
    {
        if (torn_down_)
        {
            return;
        }
        torn_down_ = true;
        loop_.Stop();
        session_->Finalize();
        srt_cleanup();
    }

    // Declaration order = destruction order REVERSED. Destruction must
    // run: session_ first (its srt teardown may still fire the send
    // callback into client_), then client_ (its Teardown needs the loop's
    // epoll), then loop_ last. Hence: loop_ declared first, session_ last.
    EventLoop                          loop_;
    std::unique_ptr<WTClient>          client_;
    std::unique_ptr<SubscriberSession> session_ = std::make_unique<SubscriberSession>(SubscriberSessionParams{});

    bool ever_started_ = false;
    bool start_failed_ = false;
    bool stopped_      = false;
    bool torn_down_    = false;

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

} // namespace websrt
