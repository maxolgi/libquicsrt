#include "srt_session/subscriber.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

namespace websrt
{

namespace
{

// Buffer depth in packets, mirroring the browser receiver
// (crates/srt-wasm/src/lib.rs new_inner): enough for the TSBPD window at
// 20 packets per millisecond, both directions, clamped to the ingest range.
int BufferPackets(int latency_ms)
{
    const long long needed = static_cast<long long>(latency_ms) * 40;
    return static_cast<int>(std::clamp(needed, 8192LL, 320000LL));
}

// SRTO_SNDBUF/SRTO_RCVBUF are in BYTES; srt converts with MSS - UDP header
// (1500 - 28 = 1472) - see the M0 loopback harness.
constexpr int kBytesPerPacket = 1500 - 28;

// Upper bound of one bounded receive slice: keeps Read responsive to a
// Stop() from another thread without busy-waiting.
constexpr int kRecvSliceMs = 250;

// Margin added to the TSBPD latency when draining a gracefully-ended
// stream: the receiver may still hold up to one latency window of
// not-yet-releasable packets.
constexpr int kDrainMarginMs = 1000;

bool SetOptInt(SRTSOCKET u, SRT_SOCKOPT opt, int value)
{
    return srt_setsockopt(u, 0, opt, &value, sizeof value) != SRT_ERROR;
}

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
    : params_(params)
{
    // Fake gateway address (10.220.0.1:5556 - M0's caller label). It is
    // only a label: dispatch happens by destination socket id inside the
    // dedicated multiplexer, never by address.
    std::memset(&peer_label_, 0, sizeof peer_label_);
    peer_label_.sin_family      = AF_INET;
    peer_label_.sin_port        = htons(5556);
    peer_label_.sin_addr.s_addr = inet_addr("10.220.0.1");
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
    params_ = params;
}

// ---- loop thread: WTClientListener ----

void SubscriberSession::OnSessionReady()
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        wt_ready_ = true;
        if (local_stop_ || state_ == State::kClosed)
        {
            // Stop() raced the WT handshake; tear down on the spot. The
            // WTClient fires OnSessionClosed for its own part.
            return;
        }
    }

    std::string error;
    if (!StartListener(&error))
    {
        // Record the srt-side failure, then close the WT session; the
        // resulting OnSessionClosed finalizes with this reason.
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (close_reason_.empty())
            {
                close_reason_ = error;
                graceful_     = false;
            }
        }
        client_->Close();
        return;
    }

    cv_.notify_all();
}

void SubscriberSession::OnSessionClosed(const std::string& reason)
{
    Finalize(reason, ReasonIsCleanEnd(reason));
}

void SubscriberSession::OnDatagram(const std::uint8_t* data, std::size_t size)
{
    // Inject under the mutex: Finalize() nulls vt_ under the same mutex,
    // which is what guarantees no injection can pass the transport release.
    const std::lock_guard<std::mutex> lock(mutex_);
    if (vt_ == nullptr || state_ == State::kClosed)
    {
        return;
    }
    srt_virtual_transport_inject(vt_,
                                 reinterpret_cast<const char*>(data),
                                 static_cast<int>(size),
                                 reinterpret_cast<const sockaddr*>(&peer_label_),
                                 static_cast<int>(sizeof peer_label_));
}

// ---- accept thread ----

void SubscriberSession::AcceptThread(SRTSOCKET listener)
{
    sockaddr_storage peer;
    int              peer_len = sizeof peer;
    const SRTSOCKET  accepted = srt_accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_len);

    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::kListening)
        {
            // Finalize raced us: it already closed (or is closing) the
            // session and cannot know about this socket.
            if (accepted != SRT_INVALID_SOCK)
            {
                srt_close(accepted);
            }
            return;
        }
        if (accepted == SRT_INVALID_SOCK)
        {
            // The listener was closed underneath us (teardown) - the
            // closing thread records the reason.
            return;
        }
        accepted_ = accepted;
        state_    = State::kConnected;
    }
    cv_.notify_all();
}

// ---- srt threads: virtual transport send callback ----

int SubscriberSession::VtSendThunk(
    void* opaq, const char* data, int len, const struct sockaddr* /*dst*/, int /*dstlen*/)
{
    return static_cast<SubscriberSession*>(opaq)->VtSend(data, len);
}

int SubscriberSession::VtSend(const char* data, int len)
{
    // Runs on srt's internal threads. Touches nothing but the thread-safe
    // WTClient: a full queue or a gone session returns false, which we
    // report as a send failure - UDP-like loss that srt retransmits. Never
    // block an srt thread. Once the session is closed (drain window), the
    // WT session is gone for good: swallow the send and claim success so
    // srt's control machinery stays quiet instead of erroring the socket
    // out from under the drain. (Relaxed atomic: a stale false only risks
    // one send attempt into the closed client, which fails harmlessly.)
    if (closed_.load(std::memory_order_relaxed))
    {
        return len;
    }
    return client_->SendDatagram(data, static_cast<std::size_t>(len)) ? len : -1;
}

// ---- listener construction (loop thread) ----

bool SubscriberSession::StartListener(std::string* error)
{
    SRTSOCKET            listener = SRT_INVALID_SOCK;
    SRTVirtualTransport* vt       = nullptr;

    const int rtt_ms = [this]
    {
        const auto rtt_us = client_->SmoothedRtt().count();
        if (rtt_us <= 0)
        {
            return 0; // unmeasured: leave srt's default estimator
        }
        // SRTO_INITIAL_RTT bounds: 1..30000 ms.
        return static_cast<int>(std::clamp<long long>(rtt_us / 1000, 1, 30000));
    }();

    do
    {
        listener = srt_create_socket();
        if (listener == SRT_INVALID_SOCK)
        {
            *error = std::string("srt_create_socket: ") + srt_getlasterror_str();
            break;
        }

        // SRTO_TRANSTYPE first: it resets the latency/payload presets.
        bool      ok       = SetOptInt(listener, SRTO_TRANSTYPE, SRTT_LIVE);
        const int buf_pkts = BufferPackets(params_.tsbpd_latency_ms);
        ok                 = ok && SetOptInt(listener, SRTO_PAYLOADSIZE, params_.payload_size);
        ok                 = ok && SetOptInt(listener, SRTO_RCVLATENCY, params_.tsbpd_latency_ms);
        ok                 = ok && SetOptInt(listener, SRTO_PEERLATENCY, params_.tsbpd_latency_ms);
        // SRTO_SNDBUF/SRTO_RCVBUF take an int (bytes): 320000 pkts x 1472
        // is ~470 MB, but BufferPackets' floor keeps us well inside int.
        ok = ok && SetOptInt(listener, SRTO_SNDBUF, buf_pkts * kBytesPerPacket);
        ok = ok && SetOptInt(listener, SRTO_RCVBUF, buf_pkts * kBytesPerPacket);
        ok = ok && SetOptInt(listener, SRTO_SKIP_INDUCTION, 1);
        if (rtt_ms > 0)
        {
            ok = ok && SetOptInt(listener, SRTO_INITIAL_RTT, rtt_ms);
        }
        if (!ok)
        {
            *error = std::string("srt socket option: ") + srt_getlasterror_str();
            break;
        }

        vt = srt_attach_virtual_transport(listener, VtSendThunk, this);
        if (vt == nullptr)
        {
            *error = std::string("srt_attach_virtual_transport: ") + srt_getlasterror_str();
            break;
        }

        // No bind: the fork synthesizes the listener label and every
        // attached socket owns a dedicated multiplexer.
        if (srt_listen(listener, 5) == SRT_ERROR)
        {
            *error = std::string("srt_listen: ") + srt_getlasterror_str();
            break;
        }
    } while (false);

    if (!error->empty())
    {
        if (vt != nullptr)
        {
            srt_virtual_transport_release(vt);
        }
        if (listener != SRT_INVALID_SOCK)
        {
            srt_close(listener);
        }
        return false;
    }

    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (local_stop_ || state_ == State::kClosed)
        {
            // Stop() raced the listener setup.
        }
        else
        {
            listener_ = listener;
            vt_       = vt;
            state_    = State::kListening;
            listener  = SRT_INVALID_SOCK; // ownership moved
            vt        = nullptr;
        }
    }

    if (listener != SRT_INVALID_SOCK)
    {
        // Raced a stop: undo everything just built.
        srt_close(listener);
        srt_virtual_transport_release(vt);
        return false;
    }

    try
    {
        accept_thread_ = std::thread([this, listener = listener_] { AcceptThread(listener); });
    }
    catch (...)
    {
        // Thread spawn failed: tear the listener down and finalize with an
        // error so Start surfaces it.
        const std::lock_guard<std::mutex> lock(mutex_);
        if (close_reason_.empty())
        {
            close_reason_ = "accept thread spawn failed";
        }
        return false;
    }
    return true;
}

// ---- app thread ----

bool SubscriberSession::WaitEstablished(int timeout_ms, std::string* why)
{
    const auto                   deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;)
    {
        if (state_ == State::kConnected)
        {
            return true;
        }
        if (state_ == State::kClosed)
        {
            if (why != nullptr)
            {
                *why = close_reason_;
            }
            return false;
        }
        if (cv_.wait_until(lock, deadline) == std::cv_status::timeout)
        {
            if (state_ == State::kConnected)
            {
                return true;
            }
            if (state_ == State::kClosed)
            {
                if (why != nullptr)
                {
                    *why = close_reason_;
                }
                return false;
            }
            if (why != nullptr)
            {
                why->clear();
            }
            return false; // timed out, session still pending
        }
    }
}

bool SubscriberSession::WaitClosed(int timeout_ms)
{
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return state_ == State::kClosed; });
    return state_ == State::kClosed;
}

int SubscriberSession::Read(char* buf, int len, int timeout_ms)
{
    if (buf == nullptr || len <= 0)
    {
        return kReadInvalidArg;
    }
    timeout_ms          = std::max(timeout_ms, 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;)
    {
        SRTSOCKET sock = SRT_INVALID_SOCK;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            switch (state_)
            {
            case State::kClosed:
                // A graceful peer end keeps the accepted socket open for
                // the TSBPD drain; everything else ends here. Local stops
                // never drain (Stop must stay prompt).
                if (!graceful_ || local_stop_ || accepted_ == SRT_INVALID_SOCK ||
                    std::chrono::steady_clock::now() >= drain_deadline_)
                {
                    return graceful_ ? kReadEnd : kReadClosed;
                }
                sock = accepted_; // drain mode
                break;
            case State::kConnected:
                sock = accepted_;
                break;
            case State::kIdle:
            case State::kListening:
                break;
            }
        }

        if (sock == SRT_INVALID_SOCK)
        {
            // Handshake not complete yet: wait for the accept thread, a
            // close, or the deadline.
            std::unique_lock<std::mutex> lock(mutex_);
            const bool                   timed_out = !cv_.wait_until(
                lock, deadline, [this] { return state_ == State::kConnected || state_ == State::kClosed; });
            if (timed_out && state_ != State::kConnected && state_ != State::kClosed)
            {
                return kReadTimeout;
            }
            continue;
        }

        // One bounded receive slice, state re-checked between slices.
        const auto now = std::chrono::steady_clock::now();
        int slice = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        slice     = std::min(slice, kRecvSliceMs);
        slice     = std::max(slice, 0);
        if (!SetOptInt(sock, SRTO_RCVTIMEO, slice))
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (close_reason_.empty())
            {
                close_reason_ = std::string("SRTO_RCVTIMEO: ") + srt_getlasterror_str();
            }
            return kReadError;
        }

        const int n = srt_recvmsg(sock, buf, len);
        if (n > 0)
        {
            return n;
        }

        const int err = srt_getlasterror(nullptr);
        if (err == SRT_ETIMEOUT)
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return kReadTimeout;
            }
            // Drain mode: after a quiet slice, an empty receive buffer is
            // the definitive end - nothing new can arrive once the session
            // closed. (RCVDATA counts buffered-but-not-yet-releasable
            // packets too, so 0 really means the TSBPD holds nothing.)
            bool draining = false;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                draining = state_ == State::kClosed && accepted_ == sock;
            }
            if (draining)
            {
                int pending = 0;
                int pend_sz = static_cast<int>(sizeof pending);
                if (srt_getsockopt(sock, 0, SRTO_RCVDATA, &pending, &pend_sz) == 0 && pending == 0)
                {
                    return kReadEnd;
                }
            }
            continue; // slice elapsed; re-check the session state first
        }
        if (err == SRT_ESCLOSED || err == SRT_ECONNLOST || err == SRT_ENOCONN)
        {
            // The stream ended. If the close is already recorded, report
            // the definitive terminal result; otherwise the next Read (or
            // a concurrent WaitEstablished) sees it.
            const std::lock_guard<std::mutex> lock(mutex_);
            if (state_ == State::kClosed)
            {
                return graceful_ ? kReadEnd : kReadClosed;
            }
            return kReadEnd;
        }

        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (close_reason_.empty())
            {
                close_reason_ = std::string("srt_recvmsg: ") + srt_getlasterror_str();
            }
        }
        return kReadError;
    }
}

bool SubscriberSession::Stats(SRT_TRACEBSTATS* out)
{
    SRTSOCKET sock = SRT_INVALID_SOCK;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::kConnected)
        {
            return false;
        }
        sock = accepted_;
    }
    return srt_bstats(sock, out, 0) == 0;
}

void SubscriberSession::RequestClose()
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::kClosed)
        {
            return;
        }
        local_stop_ = true;
    }

    // Graceful WT shutdown; OnSessionClosed (loop thread) finalizes the
    // srt side with reason "closed locally".
    client_->Close();

    // Belt and suspenders: when the WT layer never established (Connect
    // rejected, loop never ran), no OnSessionClosed will arrive. Finalize
    // runs WITHOUT the mutex (it takes it itself; racing Finalizes are
    // made safe by its idempotent state check).
    bool need_finalize = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        need_finalize = state_ != State::kClosed && !wt_ready_;
    }
    if (need_finalize)
    {
        Finalize("stopped locally", true);
    }
}

// ---- teardown ----

void SubscriberSession::Finalize()
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::kClosed)
        {
            local_stop_ = true; // a local stop never defers the teardown
        }
    }
    Finalize("stopped locally", true);
    CompleteDeferredTeardown();
}

void SubscriberSession::CompleteDeferredTeardown()
{
    SRTSOCKET            accepted = SRT_INVALID_SOCK;
    SRTVirtualTransport* vt       = nullptr;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        accepted = std::exchange(accepted_, SRT_INVALID_SOCK);
        vt       = std::exchange(vt_, nullptr);
    }
    // Same ordering rule as Finalize below: the transport handle outlives
    // the close that sends the shutdown through it.
    if (accepted != SRT_INVALID_SOCK)
    {
        srt_close(accepted);
    }
    if (vt != nullptr)
    {
        srt_virtual_transport_release(vt);
    }
}

void SubscriberSession::Finalize(const std::string& reason, bool graceful)
{
    SRTSOCKET            listener           = SRT_INVALID_SOCK;
    SRTSOCKET            accepted           = SRT_INVALID_SOCK;
    SRTVirtualTransport* vt                 = nullptr;
    bool                 have_accept_thread = false;
    bool                 defer_drain        = false;

    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::kClosed)
        {
            return;
        }
        // An srt setup error recorded earlier is the real reason; keep it.
        if (close_reason_.empty())
        {
            close_reason_ = reason;
            graceful_     = graceful;
        }
        // Terminal FIRST: wakes Read/WaitEstablished and parks OnDatagram
        // (which re-checks state_ under the lock).
        state_ = State::kClosed;
        closed_.store(true, std::memory_order_relaxed);
        listener           = std::exchange(listener_, SRT_INVALID_SOCK);
        have_accept_thread = accept_thread_.joinable();
        // A graceful peer end keeps the accepted socket (and with it the
        // transport) for the TSBPD drain - see the header.
        defer_drain = graceful && !local_stop_ && accepted_ != SRT_INVALID_SOCK;
        if (defer_drain)
        {
            drain_deadline_ =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(params_.tsbpd_latency_ms + kDrainMarginMs);
        }
        else
        {
            accepted = std::exchange(accepted_, SRT_INVALID_SOCK);
            vt       = std::exchange(vt_, nullptr);
        }
        cv_.notify_all();
    }

    // srt teardown WITHOUT the mutex (the accept thread needs it to park
    // the accepted socket). Order: accepted socket first, then the
    // listener (closing it wakes a blocked srt_accept), then join, then
    // release the transport handle - srt_close sends the shutdown through
    // the send callback, so the transport must outlive both closes.
    if (accepted != SRT_INVALID_SOCK)
    {
        srt_close(accepted);
    }
    if (listener != SRT_INVALID_SOCK)
    {
        srt_close(listener);
    }
    if (have_accept_thread)
    {
        accept_thread_.join();
    }
    if (vt != nullptr)
    {
        srt_virtual_transport_release(vt);
    }
}

bool SubscriberSession::IsClosed() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::kClosed;
}

bool SubscriberSession::ClosedGracefully() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::kClosed && graceful_;
}

std::string SubscriberSession::close_reason() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return close_reason_;
}

} // namespace websrt
