#include "srt_session/subscriber.h"

#include <algorithm>

namespace websrt
{

namespace
{

// The reasons WTClient reports for a deliberately or peer-initiated clean
// end; everything else (TLS failure, non-200 CONNECT, socket errors) is an
// error from the subscriber's point of view.
bool ReasonIsCleanEnd(const std::string& reason)
{
    return reason == "closed locally" || reason == "connection closed" ||
           reason == "closed before the connection came up";
}

} // namespace

SubscriberSession::SubscriberSession(SubscriberSessionParams params)
    : core_(params, &SinkThunk, this)
{
}

SubscriberSession::~SubscriberSession()
{
    Finalize();
}

void SubscriberSession::AttachClient(WTClient& client)
{
    client_ = &client;
}

void SubscriberSession::Configure(const SubscriberSessionParams& params)
{
    core_.Configure(params);
}

// ---- loop thread: WTClientListener ----

void SubscriberSession::OnSessionReady()
{
    wt_ready_.store(true, std::memory_order_relaxed);

    // Automatic RTT seed: the WT layer's smoothed RTT (bounds: 1..30000
    // ms; 0 = unmeasured, leave srt's default estimator). A nonzero
    // params.initial_rtt_ms overrides it - the core applies the
    // preference (InitialRttSeedMs).
    const auto rtt_us      = client_->SmoothedRtt().count();
    const int  measured_ms = rtt_us > 0
                                 ? static_cast<int>(std::clamp<long long>(rtt_us / 1000, 1, 30000))
                                 : 0;

    std::string error;
    if (!core_.Start(measured_ms, &error))
    {
        // Either a genuine srt setup failure (Start recorded it; the
        // first-wins rule keeps it ahead of the close reason below) or a
        // raced local stop (unwound silently). Closing the WT session
        // makes OnSessionClosed finalize the core either way.
        client_->Close();
    }
}

void SubscriberSession::OnSessionClosed(const std::string& reason)
{
    core_.Shutdown(reason, ReasonIsCleanEnd(reason));
}

void SubscriberSession::OnDatagram(const std::uint8_t* data, std::size_t size)
{
    core_.InjectDatagram(data, size);
}

// ---- srt threads: ReceiverCore sink ----

int SubscriberSession::SinkThunk(void* opaq, const char* data, int len)
{
    // Runs on srt's internal threads; touches nothing but the thread-safe
    // WTClient. A full queue or a gone session returns false - UDP-like
    // loss that srt retransmits. Never block an srt thread.
    const auto* self = static_cast<const SubscriberSession*>(opaq);
    return self->client_->SendDatagram(data, static_cast<std::size_t>(len)) ? len : -1;
}

// ---- app thread ----

bool SubscriberSession::WaitEstablished(int timeout_ms, std::string* why)
{
    return core_.WaitEstablished(timeout_ms, why);
}

bool SubscriberSession::WaitClosed(int timeout_ms)
{
    return core_.WaitClosed(timeout_ms);
}

int SubscriberSession::Read(char* buf, int len, int timeout_ms)
{
    return core_.Read(buf, len, timeout_ms);
}

bool SubscriberSession::Stats(SRT_TRACEBSTATS* out)
{
    return core_.Stats(out);
}

void SubscriberSession::RequestClose()
{
    if (core_.IsClosed())
    {
        return;
    }
    core_.MarkLocalStop();

    // Graceful WT shutdown; OnSessionClosed (loop thread) finalizes the
    // core with reason "closed locally".
    client_->Close();

    // Belt and suspenders: when the WT layer never established (Connect
    // rejected, loop never ran), no OnSessionClosed will arrive.
    if (!wt_ready_.load(std::memory_order_relaxed))
    {
        core_.Shutdown("stopped locally", true);
    }
}

// ---- teardown ----

void SubscriberSession::Finalize()
{
    core_.Finalize();
}

bool SubscriberSession::IsClosed() const
{
    return core_.IsClosed();
}

bool SubscriberSession::ClosedGracefully() const
{
    return core_.ClosedGracefully();
}

std::string SubscriberSession::close_reason() const
{
    return core_.close_reason();
}

} // namespace websrt
