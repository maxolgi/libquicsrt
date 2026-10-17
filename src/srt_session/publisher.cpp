#include "srt_session/publisher.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

#include "srt_session/negotiate.h"

namespace websrt
{

namespace
{

// SRTO_SNDBUF/SRTO_RCVBUF are in BYTES; srt converts with MSS - UDP header
// (1500 - 28 = 1472) - see the M0 loopback harness.
constexpr int kBytesPerPacket = 1500 - 28;

// Poll slice for the End() ACK-drain wait.
constexpr int kDrainSliceMs = 10;

// Extra wait past one TSBPD latency before the End() shutdown, so the
// gateway's receiver has released (and the ring drained) everything the
// latency window still withheld when the ACK drain completed.
constexpr int kEndReleaseMarginMs = 250;

// The reasons WTClient reports for a deliberately or peer-initiated clean
// end; everything else (TLS failure, non-200 CONNECT, socket errors) is an
// error from the publisher's point of view. Same set as the subscriber.
bool ReasonIsCleanEnd(const std::string& reason)
{
    return reason == "closed locally" || reason == "connection closed" ||
           reason == "closed before the connection came up";
}

bool SetOptInt(SRTSOCKET u, SRT_SOCKOPT opt, int value)
{
    return srt_setsockopt(u, 0, opt, &value, sizeof value) != SRT_ERROR;
}

} // namespace

// ---- TsChunker ----

TsChunker::TsChunker(int payload_size)
    : payload_size_(payload_size)
{
}

void TsChunker::Feed(const void* data, std::size_t len)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    pending_.insert(pending_.end(), bytes, bytes + len);
}

std::size_t TsChunker::pending() const
{
    return pending_.size();
}

int TsChunker::NextChunkLen() const
{
    const int whole_ts = static_cast<int>(pending_.size() / kTsPacketSize);
    if (whole_ts == 0)
    {
        return 0;
    }
    // Largest whole-TS message that fits the payload: full payload-size
    // messages whenever the stream has that many whole packets, a shorter
    // 188-multiple near the end of a Write, and never a split packet.
    return std::min(whole_ts, payload_size_ / kTsPacketSize) * kTsPacketSize;
}

const std::uint8_t* TsChunker::data() const
{
    return pending_.data();
}

void TsChunker::Consume(int n)
{
    pending_.erase(pending_.begin(), pending_.begin() + n);
}

void TsChunker::TruncateTo(std::size_t size)
{
    pending_.resize(size);
}

// ---- PublisherSession ----

PublisherSession::PublisherSession(PublisherSessionParams params)
    : params_(params)
    , chunker_(params.payload_size)
{
    // Fake gateway address (the subscriber's caller label). It is only a
    // label: dispatch happens by destination socket id inside the
    // dedicated multiplexer, never by address.
    std::memset(&peer_label_, 0, sizeof peer_label_);
    peer_label_.sin_family      = AF_INET;
    peer_label_.sin_port        = htons(5556);
    peer_label_.sin_addr.s_addr = inet_addr("10.220.0.1");
}

PublisherSession::~PublisherSession()
{
    Finalize();
}

void PublisherSession::AttachClient(WTClient& client)
{
    client_ = &client;
}

void PublisherSession::Configure(const PublisherSessionParams& params)
{
    params_  = params;
    chunker_ = TsChunker(params_.payload_size);
}

// ---- loop thread: WTClientListener ----

void PublisherSession::OnSessionReady()
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        wt_ready_ = true;
        if (local_stop_ || state_ == State::kClosed)
        {
            return;
        }
    }

    std::string error;
    if (!StartListener(&error))
    {
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

void PublisherSession::OnSessionClosed(const std::string& reason)
{
    Finalize(reason, ReasonIsCleanEnd(reason));
}

void PublisherSession::OnDatagram(const std::uint8_t* data, std::size_t size)
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

void PublisherSession::AcceptThread(SRTSOCKET listener)
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

        // The handshake negotiates min(gateway, listener) payload: the
        // chunker must frame to the negotiated size, not the configured
        // one (M0's negotiated-payload readback; see negotiate.h).
        chunker_  = TsChunker(NegotiatePayload(accepted, params_.payload_size));
        accepted_ = accepted;
        state_    = State::kConnected;
    }
    cv_.notify_all();
}

// ---- srt threads: virtual transport send callback ----

int PublisherSession::VtSendThunk(void* opaq, const char* data, int len, const struct sockaddr* /*dst*/, int /*dstlen*/)
{
    return static_cast<PublisherSession*>(opaq)->VtSend(data, len);
}

int PublisherSession::VtSend(const char* data, int len)
{
    // Runs on srt's internal threads. Touches nothing but the thread-safe
    // WTClient: a full queue or a gone session returns false, which we
    // report as a send failure - UDP-like loss that srt retransmits. Never
    // block an srt thread. Once the session is closed, the WT session is
    // gone for good: swallow the send and claim success so srt's control
    // machinery stays quiet. (Relaxed atomic: a stale false only risks one
    // send attempt into the closed client, which fails harmlessly.)
    if (closed_.load(std::memory_order_relaxed))
    {
        return len;
    }
    return client_->SendDatagram(data, static_cast<std::size_t>(len)) ? len : -1;
}

// ---- listener construction (loop thread) ----

bool PublisherSession::StartListener(std::string* error)
{
    SRTSOCKET            listener = SRT_INVALID_SOCK;
    SRTVirtualTransport* vt       = nullptr;

    const int rtt_ms = [this]
    {
        const auto rtt_us      = client_->SmoothedRtt().count();
        const int  measured_ms = rtt_us > 0 ? static_cast<int>(std::clamp<long long>(rtt_us / 1000, 1, 30000))
                                            : 0; // unmeasured: leave srt's default estimator
        // An explicit params_.initial_rtt_ms overrides the measured seed.
        return InitialRttSeedMs(params_.initial_rtt_ms, measured_ms);
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
        // SNDBUF carries the whole retransmit window for the sender role;
        // both directions stay latency-scaled like the browser publisher.
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
        // attached socket owns a dedicated multiplexer. The gateway
        // initiates the moment the CONNECT is accepted, so the accept
        // thread must be parked here.
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

bool PublisherSession::WaitEstablished(int timeout_ms, std::string* why)
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

bool PublisherSession::WaitClosed(int timeout_ms)
{
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return state_ == State::kClosed; });
    return state_ == State::kClosed;
}

int PublisherSession::SendOne(const std::uint8_t* buf, int len, int timeout_ms)
{
    // One live message. SRTO_SNDTIMEO bounds the blocking part (a full
    // send buffer); Finalize's socket close wakes an in-flight send with
    // SRT_ECONNLOST, so Stop stays prompt without send slicing.
    timeout_ms     = std::max(timeout_ms, 1);
    SRTSOCKET sock = SRT_INVALID_SOCK;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::kConnected)
        {
            return SRT_ERROR; // caller maps via state, not last error
        }
        sock = accepted_;
    }
    if (sock == SRT_INVALID_SOCK)
    {
        return SRT_ERROR;
    }
    if (!SetOptInt(sock, SRTO_SNDTIMEO, timeout_ms))
    {
        return SRT_ERROR;
    }
    return srt_sendmsg2(sock, reinterpret_cast<const char*>(buf), len, nullptr);
}

int PublisherSession::Write(const void* data, int len, int timeout_ms)
{
    if (data == nullptr || len <= 0)
    {
        return kWriteInvalidArg;
    }
    timeout_ms          = std::max(timeout_ms, 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::kClosed)
        {
            return kWriteClosed;
        }
        if (state_ != State::kConnected)
        {
            return kWriteNotStarted;
        }
    }

    const std::size_t before = chunker_.pending();
    chunker_.Feed(data, static_cast<std::size_t>(len));

    int  sent_total = 0; // bytes that left in complete messages this call
    int  error      = 0;
    bool drained    = false; // every complete message went out
    for (bool first = true;; first = false)
    {
        const int n = chunker_.NextChunkLen();
        if (n == 0)
        {
            drained = true; // only the sub-188 tail remains
            break;
        }
        // Budget: the first send always gets a slice (poll semantics),
        // later ones only while the deadline has room.
        const auto now = std::chrono::steady_clock::now();
        const int  slice_ms =
            static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        if (!first && slice_ms <= 0)
        {
            break;
        }

        const int r = SendOne(chunker_.data(), n, slice_ms);
        if (r == n)
        {
            chunker_.Consume(n);
            sent_total += n;
            continue;
        }

        // Send failure. state-change wins (the session closed under us);
        // otherwise srt's error class picks the code.
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (state_ == State::kClosed)
            {
                error = SRT_ECONNLOST;
            }
        }
        if (error == 0)
        {
            error = srt_getlasterror(nullptr);
            const std::lock_guard<std::mutex> lock(mutex_);
            if (close_reason_.empty())
            {
                close_reason_ = std::string("srt_sendmsg2: ") + srt_getlasterror_str();
            }
        }
        break;
    }

    if (drained && error == 0)
    {
        // Everything that could go out went out: all len bytes accepted
        // (a sub-188 tail may stay buffered for the next Write / End).
        return len;
    }

    // Stopped early (deadline or failure): whatever of this call did not
    // go out is handed back - the caller resumes from data + returned.
    // `sent_total` consumed the oldest bytes first (a previous call's
    // carry, then this call's data), so only the still-unsent old carry
    // stays buffered.
    const int from_call = std::max(0, sent_total - static_cast<int>(before));
    chunker_.TruncateTo(before > static_cast<std::size_t>(sent_total) ? before - static_cast<std::size_t>(sent_total)
                                                                      : 0);
    if (from_call <= 0)
    {
        if (error == SRT_ETIMEOUT)
        {
            return kWriteTimeout;
        }
        if (error == SRT_ECONNLOST || error == SRT_ESCLOSED || error == SRT_ENOCONN)
        {
            return kWriteClosed;
        }
        return error == 0 ? kWriteTimeout : kWriteError;
    }
    return from_call;
}

// Instantaneous stats for the drain wait: plain srt_bstats reports the
// send-buffer fields as IIR-smoothed moving averages (25 ms sampling,
// dt/1000 weight, srtcore/buffer_tools.cpp), which decay exponentially
// (~x0.6 per 500 ms) once the buffer is truly empty and kept this wait
// pinned seconds past the real drain - the End(5 s) timeout was spent
// waiting on an average, not on the buffer.
bool PublisherSession::StatsInstant(SRT_TRACEBSTATS* out)
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
    return srt_bistats(sock, out, 0, 1) == 0;
}

bool PublisherSession::WaitSendBufferDrained(int timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;)
    {
        SRT_TRACEBSTATS perf;
        if (!StatsInstant(&perf))
        {
            return false; // socket gone
        }
        // pktSndBuf: data packets in the send buffer not yet ACKed.
        if (perf.pktSndBuf == 0 && perf.byteSndBuf == 0)
        {
            return true;
        }
        if (std::chrono::steady_clock::now() + std::chrono::milliseconds(kDrainSliceMs) >= deadline)
        {
            return perf.pktSndBuf == 0 && perf.byteSndBuf == 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kDrainSliceMs));
    }
}

bool PublisherSession::End(int timeout_ms)
{
    timeout_ms           = std::max(timeout_ms, 0);
    const auto deadline  = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    auto       remaining = [&deadline]
    {
        const auto now = std::chrono::steady_clock::now();
        return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
    };

    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::kClosed)
        {
            return graceful_; // already ended; End is idempotent
        }
        if (state_ != State::kConnected)
        {
            if (close_reason_.empty())
            {
                close_reason_ = "End before the session was established";
            }
            return false;
        }
    }

    // (1) Flush every complete message, then the sub-188 tail as one
    // final short message (a TS source is 188-aligned, so the tail only
    // exists for byte streams that are not).
    for (;;)
    {
        const int n = chunker_.NextChunkLen();
        if (n == 0)
        {
            break;
        }
        if (SendOne(chunker_.data(), n, std::max(remaining(), 1)) != n)
        {
            return false; // close_reason_ set by the send path
        }
        chunker_.Consume(n);
    }
    if (chunker_.pending() > 0)
    {
        const int tail = static_cast<int>(chunker_.pending());
        if (SendOne(chunker_.data(), tail, std::max(remaining(), 1)) != tail)
        {
            return false;
        }
        chunker_.Consume(tail);
    }

    // (2) Wait until the gateway ACKed everything: srt_close on a LIVE
    // socket does not linger (SRTO_LINGER defaults off in LIVE), so
    // unACKed data would be dropped by the close.
    if (!WaitSendBufferDrained(std::max(remaining(), 0)))
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (close_reason_.empty())
            {
                close_reason_ = "End: send buffer did not drain in time";
            }
        }
        return false;
    }

    // (2b) Let the gateway's receive-side TSBPD release the ACKed window
    // at its natural deadlines before the shutdown flushes whatever it
    // still withholds: a full-latency flush arrives as one burst that
    // both overflow-drops in the gateway's publish channel and stacks
    // the tail into the viewer-close window. Waiting just over one
    // latency turns the shutdown's own drain into a no-op.
    {
        const int release_ms = params_.tsbpd_latency_ms + kEndReleaseMarginMs;
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(release_ms, std::max(remaining(), 0))));
    }

    // (3) Close the accepted socket while the WT session is still open:
    // the SRT shutdown rides the transport, the gateway's initiator
    // drains its TSBPD-held tail into the ring, and then it closes the
    // WT session itself.
    SRTSOCKET accepted = SRT_INVALID_SOCK;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        accepted = std::exchange(accepted_, SRT_INVALID_SOCK);
    }
    if (accepted != SRT_INVALID_SOCK)
    {
        srt_close(accepted);
    }

    // (4) The gateway closes the WT session once its receive side is
    // flushed (bounded by ~2x TSBPD latency). Fall back to a local WT
    // close if it stalls - the tail is already ACKed, so the fallback
    // only risks the gateway's not-yet-released TSBPD window.
    if (WaitClosed(std::max(remaining(), 0)))
    {
        return ClosedGracefully();
    }
    RequestClose();
    WaitClosed(2000);
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (close_reason_.empty() && !(state_ == State::kClosed && graceful_))
        {
            close_reason_ = "End: gateway did not close after shutdown";
        }
    }
    return IsClosed();
}

bool PublisherSession::Stats(SRT_TRACEBSTATS* out)
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

void PublisherSession::RequestClose()
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
    // rejected, loop never ran), no OnSessionClosed will arrive.
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

void PublisherSession::Finalize()
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::kClosed)
        {
            local_stop_ = true;
        }
    }
    Finalize("stopped locally", true);
}

void PublisherSession::Finalize(const std::string& reason, bool graceful)
{
    SRTSOCKET            listener           = SRT_INVALID_SOCK;
    SRTSOCKET            accepted           = SRT_INVALID_SOCK;
    SRTVirtualTransport* vt                 = nullptr;
    bool                 have_accept_thread = false;

    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::kClosed)
        {
            return;
        }
        if (close_reason_.empty())
        {
            close_reason_ = reason;
            graceful_     = graceful;
        }
        // Terminal FIRST: wakes WaitEstablished/End and parks OnDatagram.
        state_ = State::kClosed;
        closed_.store(true, std::memory_order_relaxed);
        listener           = std::exchange(listener_, SRT_INVALID_SOCK);
        accepted           = std::exchange(accepted_, SRT_INVALID_SOCK);
        vt                 = std::exchange(vt_, nullptr);
        have_accept_thread = accept_thread_.joinable();
        // No deferred drain here (the subscriber's TSBPD tail lesson does
        // not apply: a publisher receives only ACK/NAK).
        cv_.notify_all();
    }

    // srt teardown WITHOUT the mutex (the accept thread needs it to park
    // the accepted socket). Order: accepted socket first (End may have
    // closed it already - srt_close is idempotent on a closed handle),
    // then the listener (closing it wakes a blocked srt_accept), then
    // join, then release the transport handle - both closes may send the
    // shutdown through the send callback, so the transport outlives them.
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

bool PublisherSession::IsClosed() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::kClosed;
}

bool PublisherSession::ClosedGracefully() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::kClosed && graceful_;
}

std::string PublisherSession::close_reason() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return close_reason_;
}

} // namespace websrt
