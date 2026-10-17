// Publisher façade (libwebsrt public API): owns the EventLoop, one
// WTClient, and the PublisherSession, and exposes the blocking,
// exception-free surface declared in include/websrt/publisher.hpp.
// Structurally the Subscriber façade with the data plane reversed -
// see src/websrt/subscriber.cpp for the construction-order and
// teardown-order rationale (session first, client, loop last; the
// srt library reference is dropped only in Teardown()).

#include "websrt/publisher.hpp"

#include <algorithm>
#include <climits>
#include <mutex>
#include <string>
#include <utility>

#include "core/event_loop.h"
#include "srt.h"
#include "srt_session/publisher.h"
#include "websrt/resolve.h"
#include "wt/client.h"

namespace websrt
{

namespace
{

// Bound for how long Stop()/End() waits for the graceful WT close to
// complete before stopping the loop underneath it. Longer than the
// subscriber's 3000: the publisher's drain also gives the SRT sender time
// to flush its still-unsent buffer before the WT session dies, instead of
// truncating the stream tail.
constexpr int kStopDrainMs = 5000;

} // namespace

class Publisher::Impl
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

    PublisherStartStatus Start(const PublisherConfig& config)
    {
        if (ever_started_)
        {
            return kPublishAlreadyStarted;
        }

        const std::string config_error = config.validate();
        if (!config_error.empty())
        {
            SetError(config_error);
            return kPublishInvalidConfig;
        }
        ever_started_ = true;

        PublisherSessionParams params;
        params.payload_size     = config.payload_size;
        params.tsbpd_latency_ms = config.tsbpd_latency_ms;
        params.initial_rtt_ms   = config.initial_rtt_ms;
        session_->Configure(params);

        WTClient::Target target;
        target.host          = config.host;
        target.port          = config.port;
        target.path          = "/wt?publish=" + config.stream_name;
        target.cert_hash_hex = config.cert_sha256_hex;

        srt_startup(); // refcounted; paired with srt_cleanup() in Teardown()
        loop_.Start();
        // Resolve on the calling thread - see the Subscriber façade: the
        // blocking DNS wait is the OS resolver's default budget, outside
        // connect_timeout_ms.
        std::string resolve_error;
        target.resolved_ip = ResolveHostToIpv4(config.host, &resolve_error);
        if (target.resolved_ip.empty())
        {
            SetError(resolve_error);
            start_failed_ = true;
            Teardown();
            return kPublishInvalidTarget;
        }
        if (!client_->Connect(target))
        {
            SetError("target rejected (port nonzero, path nonempty, cert hash malformed)");
            start_failed_ = true;
            Teardown();
            return kPublishInvalidTarget;
        }

        std::string why;
        if (session_->WaitEstablished(config.connect_timeout_ms, &why))
        {
            SetError(std::string());
            return kPublishOk;
        }
        if (session_->IsClosed())
        {
            SetError(why.empty() ? "session closed during connect" : why);
            start_failed_ = true;
            Teardown();
            const int status = client_->HttpStatus();
            if (status != -1 && status != 200)
            {
                // 403 = the stream name is held by a live source
                // (gateway.rs: "stream name in use", first-wins).
                return kPublishRejected;
            }
            return kPublishConnectFailed;
        }

        // connect_timeout_ms elapsed: tear the half-open session down.
        SetError("connect timeout after " + std::to_string(config.connect_timeout_ms) + " ms");
        start_failed_ = true;
        session_->RequestClose();
        session_->WaitClosed(kStopDrainMs);
        Teardown();
        return kPublishTimedOut;
    }

    int Write(const void* data, std::size_t len, int timeout_ms)
    {
        if (!ever_started_)
        {
            return kWriteNotStarted;
        }
        if (start_failed_)
        {
            return kWriteClosed; // Start failed; LastError() has the reason
        }
        if (data == nullptr || len == 0)
        {
            return kWriteInvalidArg;
        }
        const int capped = static_cast<int>(std::min<std::size_t>(len, INT_MAX));
        const int result = session_->Write(data, capped, timeout_ms);
        if (result < 0 && result != kWriteTimeout)
        {
            SetError(session_->close_reason());
        }
        return result;
    }

    bool End(int timeout_ms)
    {
        if (!ever_started_ || start_failed_)
        {
            return false;
        }
        if (!session_->End(timeout_ms))
        {
            SetError(session_->close_reason());
            return false;
        }
        return true;
    }

    StatsStatus Stats(PublisherStats* out)
    {
        if (out == nullptr)
        {
            return kStatsNotConnected;
        }
        *out = PublisherStats{};
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
        out->packets_sent          = perf.pktSentUniqueTotal;
        out->bytes_sent            = perf.byteSentUniqueTotal;
        out->packets_retransmitted = perf.pktRetransTotal;
        out->packets_received      = perf.pktRecvUniqueTotal;
        out->bytes_received        = perf.byteRecvUniqueTotal;
        out->acks_received         = perf.pktRecvACKTotal;
        out->naks_received         = perf.pktRecvNAKTotal;

        out->rtt_ms            = perf.msRTT;
        out->send_rate_mbps    = perf.mbpsSendRate;
        out->bandwidth_mbps    = perf.mbpsBandwidth;
        out->flow_window       = perf.pktFlowWindow;
        out->congestion_window = perf.pktCongestionWindow;
        out->flight_size       = perf.pktFlightSize;
        out->packets_buffered  = perf.pktSndBuf;
        out->bytes_buffered    = perf.byteSndBuf;
        out->ms_tsbpd_delay    = perf.msSndTsbPdDelay;
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
    EventLoop                         loop_;
    std::unique_ptr<WTClient>         client_;
    std::unique_ptr<PublisherSession> session_ = std::make_unique<PublisherSession>(PublisherSessionParams{});

    bool ever_started_ = false;
    bool start_failed_ = false;
    bool stopped_      = false;
    bool torn_down_    = false;

    mutable std::mutex error_mutex_;
    std::string        last_error_;
};

Publisher::Publisher()
    : impl_(std::make_unique<Impl>())
{
}

Publisher::~Publisher() = default;

PublisherStartStatus Publisher::Start(const PublisherConfig& config)
{
    return impl_->Start(config);
}

int Publisher::Write(const void* data, std::size_t len, int timeout_ms)
{
    return impl_->Write(data, len, timeout_ms);
}

bool Publisher::End(int timeout_ms)
{
    return impl_->End(timeout_ms);
}

StatsStatus Publisher::Stats(PublisherStats* out)
{
    return impl_->Stats(out);
}

void Publisher::Stop()
{
    impl_->Stop();
}

std::string Publisher::LastError() const
{
    return impl_->LastError();
}

} // namespace websrt
