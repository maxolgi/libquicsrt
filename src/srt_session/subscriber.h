// SRT-over-WT subscriber session: the WebTransport adapter over the
// WT-free ReceiverCore (receiver_core.{h,cpp}). Owns no SRT state of its
// own: OnSessionReady/OnDatagram/OnSessionClosed forward into the core,
// and the core's outgoing datagrams ride the sink into
// WTClient::SendDatagram. The core carries the full receiver contract
// (listener construction, accept thread, inject, bounded reads, stats,
// the fixed teardown order) - see receiver_core.h for the threading and
// lifetime rules; this adapter adds exactly the WT specifics:
//
//   - OnSessionReady seeds SRTO_INITIAL_RTT from the WT layer's smoothed
//     RTT (lsquic_conn_get_info) and starts the core's listener.
//   - WT close reasons map to graceful/end-of-stream vs error
//     (ReasonIsCleanEnd).
//   - RequestClose closes the WT session gracefully; the resulting
//     OnSessionClosed finalizes the core with reason "closed locally".
//
// The peer label, options table, and buffer depth are the core's.

#ifndef WEBSRT_SRT_SESSION_SUBSCRIBER_H
#define WEBSRT_SRT_SESSION_SUBSCRIBER_H

#include <atomic>
#include <string>

#include "proto/constants.h"
#include "srt_session/receiver_core.h"
#include "wt/client.h"

// The fork's gated API (build with SRT_ENABLE_VIRTUAL_TRANSPORT defined).
#include "srt.h"

namespace websrt
{

using SubscriberSessionParams = ReceiverCoreParams;

class SubscriberSession final : public WTClientListener
{
public:
    // Read outcomes; positive values carry the delivered byte count, so
    // Read() returns plain int and these are its documented constants.
    // They mirror the public Subscriber's values (include/websrt/subscriber.hpp).
    enum ReadResult
    {
        kReadEnd        = 0,  // stream ended (graceful); sticky afterwards
        kReadTimeout    = -1, // no message within the timeout; call again
        kReadClosed     = -2, // session ended with an error (close_reason)
        kReadNotStarted = -3, // Read before the session was ever started
        kReadInvalidArg = -4,
        kReadError      = -5, // srt-level error (see close_reason)
    };

    // `client` must outlive this object. The session never calls Connect;
    // the owner starts the WT session and the first callback (ready or
    // closed) drives the srt side.
    //
    // Two-phase wiring: the owner constructs the session first (it is the
    // WTClient's listener), then the WTClient (it needs the listener by
    // reference), then calls AttachClient once both exist. Configure sets
    // the SRT parameters and must happen before the session goes live.
    explicit SubscriberSession(SubscriberSessionParams params);
    ~SubscriberSession();

    SubscriberSession(const SubscriberSession&)            = delete;
    SubscriberSession& operator=(const SubscriberSession&) = delete;

    void AttachClient(WTClient& client);
    void Configure(const SubscriberSessionParams& params);

    // ---- app thread ----

    // Waits until the SRT handshake completed (true), the session closed
    // (false, *why set), or timeout_ms elapsed (false, *why empty).
    bool WaitEstablished(int timeout_ms, std::string* why);

    // Waits until the session reached its terminal state. True when
    // closed, false on timeout.
    bool WaitClosed(int timeout_ms);

    // Delivers one SRT message (a TS chunk of at most the payload size)
    // into buf. len must be at least the configured payload size; see
    // ReadResult for the return contract. Bounded receive slices keep the
    // call responsive to Stop from another thread.
    int Read(char* buf, int len, int timeout_ms);

    // Snapshots the accepted socket's stats (srt_bstats, no clear).
    // False when there is no connected accepted socket.
    bool Stats(SRT_TRACEBSTATS* out);

    // Local, graceful stop: closes the WT session (which finalizes the
    // core through OnSessionClosed) and finalizes directly when the WT
    // layer never came up. Idempotent; safe from any thread.
    void RequestClose();

    // Idempotent terminal cleanup (sockets, accept thread, transport).
    // Safe with the loop still running; a no-op once closed.
    void Finalize();

    bool        IsClosed() const;
    bool        ClosedGracefully() const;
    std::string close_reason() const;

private:
    // ---- loop thread (WTClientListener) ----
    void OnSessionReady() override;
    void OnSessionClosed(const std::string& reason) override;
    void OnDatagram(const std::uint8_t* data, std::size_t size) override;

    // ---- srt threads (via ReceiverCore's sink) ----
    static int SinkThunk(void* opaq, const char* data, int len);

    WTClient*    client_ = nullptr; // set by AttachClient, pre-use
    ReceiverCore core_;

    // Set once OnSessionReady fired; gates RequestClose's direct
    // fallback (no OnSessionClosed can arrive when the WT layer never
    // established). Written on the loop thread, read on the app thread.
    std::atomic<bool> wt_ready_{false};
};

} // namespace websrt

#endif // WEBSRT_SRT_SESSION_SUBSCRIBER_H
