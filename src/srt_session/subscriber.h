// SRT-over-WT subscriber session: the native analogue of the browser-side
// srt-wasm SrtReceiver (WebSRT crates/srt-wasm/src/lib.rs). The gateway is
// the SRT initiator (caller); this session is the listener: on WT session
// ready it creates one forked-srt socket in virtual-transport mode, puts it
// into the listening state WITHOUT a bind (the fork synthesizes the label),
// and answers the HSv5 Conclusion-first handshake the gateway floods every
// ~2 ms. Incoming WT datagrams are injected into the srt receive queue with
// a stable fake source label; outgoing srt datagrams (handshake response,
// ACK, NAK) ride the send callback into WTClient::SendDatagram.
//
// Socket options are set on the listener BEFORE srt_listen (SRTO_TRANSTYPE
// first - it resets the latency/payload presets), and inherited by the
// accepted socket:
//   SRTO_TRANSTYPE      SRTT_LIVE
//   SRTO_PAYLOADSIZE    params.payload_size (1128: 6 x 188 TS packets)
//   SRTO_RCVLATENCY     params.tsbpd_latency_ms (TSBPD = max(sender, receiver))
//   SRTO_PEERLATENCY    params.tsbpd_latency_ms
//   SRTO_SNDBUF         snd packets * 1472 bytes (SRTO_SNDBUF is in BYTES)
//   SRTO_RCVBUF         rcv packets * 1472 bytes
//   SRTO_SKIP_INDUCTION 1 (listener accepts a Conclusion-first handshake)
//   SRTO_INITIAL_RTT    QUIC smoothed RTT clamped to 1..30000 ms, when
//                       measured (skip when 0)
//
// Buffer depth mirrors the browser receiver: latency-scaled packets count
// clamp(latency_ms * 40, 8192, 320000) for both directions.
//
// THREADS AND TEARDOWN ORDER (three thread domains touch this object):
//
//   1. Loop thread   - WTClientListener callbacks (OnSessionReady,
//                      OnDatagram, OnSessionClosed). All engine driving.
//   2. SRT threads   - the virtual-transport send callback (VtSend) fires
//                      from srt's internal multiplexer threads; it touches
//                      ONLY the WTClient reference (thread-safe by design)
//                      and never session state.
//   3. Accept thread - a short-lived std::thread spawned on session ready;
//                      blocks in srt_accept until the handshake completes
//                      (or the listener closes underneath it), stores the
//                      accepted socket, and exits. The gateway's handshake
//                      retransmit flood while the thread is parked in
//                      srt_accept is harmless: srt answers every copy and
//                      dedups the connection.
//   4. App thread    - Start/Read/Stats/Stop of the public Subscriber.
//
// Lifetime rules enforced here:
//   - Every shared field lives under mutex_; cv_ covers state transitions.
//     Read NEVER holds the mutex while blocking in srt receive (it would
//     starve OnDatagram's inject); it re-checks state between bounded
//     receive slices (SRTO_RCVTIMEO <= 250 ms).
//   - OnDatagram injects WITH the mutex held so that Finalize can
//     guarantee no inject happens past the transport release (they
//     serialize on mutex_ instead of racing a stale handle).
//   - The shutdown order is: (a) state -> kClosed and vt_ -> null under
//     mutex_ (waiters wake, injects stop); (b) OUTSIDE the mutex
//     srt_close(accepted), srt_close(listener) (this also wakes a blocked
//     accept), join the accept thread, then srt_virtual_transport_release.
//     Joining outside the mutex matters: the accept thread takes the mutex
//     on its success path. Sockets are closed before the transport is
//     released because srt_close sends the shutdown through the send
//     callback (the M0 loopback harness lesson).
//   - After Finalize the object is inert; the WTClient may be destroyed
//     only once no callback can fire (loop stopped or OnSessionClosed
//     returned), which the public Subscriber guarantees before its members
//     are destroyed (session first, client second, loop last).

#ifndef WEBSRT_SRT_SESSION_SUBSCRIBER_H
#define WEBSRT_SRT_SESSION_SUBSCRIBER_H

#include <arpa/inet.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "wt/client.h"

// The fork's gated API (build with SRT_ENABLE_VIRTUAL_TRANSPORT defined).
#include "srt.h"

namespace websrt
{

struct SubscriberSessionParams
{
    int payload_size     = 1128; // WebSRT constant: 6 x 188-byte TS packets
    int tsbpd_latency_ms = 120;  // gateway ingest default; TSBPD = max(sender, receiver)
    // 0 = seed from the WT layer's smoothed RTT at session-ready time.
    int initial_rtt_ms = 0;
};

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

    // Local, graceful stop: closes the WT session (which finalizes this
    // session through OnSessionClosed) and finalizes directly when the WT
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

    // ---- accept thread ----
    void AcceptThread(SRTSOCKET listener);

    // ---- srt send callback (srt internal threads) ----
    static int VtSendThunk(void* opaq, const char* data, int len, const struct sockaddr* dst, int dstlen);
    int        VtSend(const char* data, int len);

    // Builds the listener: socket, options, transport attach, listen.
    // Returns false with *error set; called from OnSessionReady.
    bool StartListener(std::string* error);

    // Terminal transition: records the reason, wakes waiters, tears the
    // srt side down. `graceful` marks locally-requested or peer-clean
    // ends. Idempotent; no lock held on entry. An earlier SetupError()
    // reason wins over `reason`. On a graceful PEER end with a live
    // accepted socket, the accepted socket and transport are kept open
    // (bounded by the TSBPD latency) so Read can drain what the receiver
    // buffer already holds - the gateway closes the session the moment
    // the ring is empty, but TSBPD still withholds the last ~latency of
    // delivered data. Finalize() (Stop/destructor) completes that
    // deferred teardown.
    void Finalize(const std::string& reason, bool graceful);

    // Closes a drain-deferred accepted socket and releases its transport
    // (no-op when nothing was deferred).
    void CompleteDeferredTeardown();

    enum class State
    {
        kIdle,      // WT session not established yet
        kListening, // srt listener up, accept thread parked
        kConnected, // SRT handshake complete (accepted socket live)
        kClosed,    // terminal
    };

    WTClient*               client_ = nullptr; // set by AttachClient, pre-use
    SubscriberSessionParams params_;

    // Fake address labeling the gateway for srt dispatch (mirrors M0's
    // caller label; the address never reaches a network, it only has to be
    // stable for the session).
    sockaddr_in peer_label_ = {};

    mutable std::mutex      mutex_;
    std::condition_variable cv_;
    State                   state_      = State::kIdle;
    bool                    wt_ready_   = false;
    bool                    local_stop_ = false;
    bool                    graceful_   = false;
    std::string             close_reason_; // first reason wins
    SRTSOCKET               listener_ = SRT_INVALID_SOCK;
    SRTSOCKET               accepted_ = SRT_INVALID_SOCK;
    SRTVirtualTransport*    vt_       = nullptr;
    std::thread             accept_thread_;

    // Drain window on a graceful peer end: Read may keep delivering the
    // accepted socket's buffered messages until this point (TSBPD latency
    // plus margin); afterwards the end is final.
    std::chrono::steady_clock::time_point drain_deadline_ = std::chrono::steady_clock::time_point::max();

    // Lock-free mirror of state_ == kClosed for the srt send callback.
    std::atomic<bool> closed_{false};
};

} // namespace websrt

#endif // WEBSRT_SRT_SESSION_SUBSCRIBER_H
