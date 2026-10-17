// WT-free SRT receiver core: the pure-SRT receiver machinery extracted
// from SubscriberSession - the native analogue of the browser-side
// srt-wasm SrtReceiver (WebSRT crates/srt-wasm/src/lib.rs). The peer is
// the SRT initiator (caller); this core is the listener: on Start it
// creates one forked-srt socket in virtual-transport mode, binds it to
// a label address, puts it into the listening state,
// and answers the HSv5 Conclusion-first handshake the initiator floods
// every ~2 ms. Incoming datagrams are injected into the srt receive
// queue with a stable fake source label; outgoing srt datagrams
// (handshake response, ACK, NAK) ride the injected datagram-sink
// callback to whatever transport the owner wired up (a WTClient, an
// engine shim, ...). Zero WebTransport includes - this is the layer an
// Emscripten build consumes directly.
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
//   SRTO_INITIAL_RTT    params.initial_rtt_ms when explicitly set, else
//                       the measured RTT clamped to 1..30000 ms
//                       (skipped when 0)
//
// Buffer depth mirrors the browser receiver: latency-scaled packets count
// clamp(latency_ms * 40, 8192, 320000) for both directions.
//
// Send path: SendMessage (the app thread, beside Read) publishes on the
// same accepted socket - the browser engine's publish direction. Each
// call splits the buffer into messages of at most the NEGOTIATED payload
// (the accept thread's HSv5 readback: effectively 940 = 5 x 188, not the
// configured 1128), each its own srt message; no remainder carries
// across calls. Returns the bytes accepted (a partial count when the
// deadline or the session gave out midway - the caller resumes from
// buf + returned), 0 for an empty buffer (nothing emitted), or a
// SendResult status. A full send buffer blocks a chunk bounded by
// SRTO_SNDTIMEO (the call's remaining deadline); Finalize's socket close
// wakes an in-flight send with SRT_ECONNLOST, so Stop stays prompt (the
// publisher's SendOne lesson).
//
// THREADS AND TEARDOWN ORDER (three thread domains touch this object):
//
//   1. Driver thread  - the transport owner's callbacks: Start (listener
//                       setup), InjectDatagram, Shutdown. For the
//                       WebTransport adapter this is the event-loop
//                       thread; an engine shim may use its own.
//   2. SRT threads    - the virtual-transport send callback (VtSend) fires
//                      from srt's internal multiplexer threads; it touches
//                      ONLY the injected sink (which must be thread-safe
//                      by contract) and never core state.
//   3. Accept thread - a short-lived std::thread spawned by Start;
//                      blocks in srt_accept until the handshake completes
//                      (or the listener closes underneath it), stores the
//                      accepted socket, and exits. The initiator's
//                      handshake retransmit flood while the thread is
//                      parked in srt_accept is harmless: srt answers every
//                      copy and dedups the connection.
//   4. App thread    - WaitEstablished/Read/SendMessage/Stats/
//                      MarkLocalStop/Finalize of the owning facade.
//
// Lifetime rules enforced here:
//   - Every shared field lives under mutex_; cv_ covers state transitions.
//     Read NEVER holds the mutex while blocking in srt receive (it would
//     starve InjectDatagram's inject); it re-checks state between bounded
//     receive slices (SRTO_RCVTIMEO <= 250 ms).
//   - InjectDatagram injects WITH the mutex held so that Finalize can
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
//   - After Shutdown/Finalize the object is inert; the transport owner
//     must guarantee no callback fires past the teardown (the WebTransport
//     adapter's WTClient outlives the session and serializes on
//     OnSessionClosed).

#ifndef WEBSRT_SRT_SESSION_RECEIVER_CORE_H
#define WEBSRT_SRT_SESSION_RECEIVER_CORE_H

#include <arpa/inet.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "proto/constants.h"

// The fork's gated API (build with SRT_ENABLE_VIRTUAL_TRANSPORT defined).
#include "srt.h"

namespace websrt
{

struct ReceiverCoreParams
{
    int payload_size     = kSrtPayloadSize; // 6 x 188 TS packets; HSv5 negotiates min(gateway, listener)
    int tsbpd_latency_ms = 120;             // gateway ingest default; TSBPD = max(sender, receiver)
    // > 0 overrides the transport-measured RTT as the seed; 0 = seed
    // from the measured RTT at Start time.
    int initial_rtt_ms = 0;
};

// Outgoing-datagram sink: the raw bytes srt wants on the wire (handshake
// response, ACK, NAK). Called on srt's internal threads - must be
// thread-safe and never block. Returns len on success or a negative
// value on failure: UDP-like loss that srt retransmits.
using DatagramSink = int (*)(void* opaq, const char* data, int len);

class ReceiverCore
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
        kReadNotStarted = -3, // Read before Start
        kReadInvalidArg = -4,
        kReadError      = -5, // srt-level error (see close_reason)
    };

    // Send outcomes; positive values carry the accepted byte count (0 is
    // not a status - an empty buffer is a no-op), so SendMessage() returns
    // plain int and these are its documented constants.
    enum SendResult
    {
        kSendTimeout       = -1, // send buffer stayed full until the deadline; call again
        kSendClosed        = -2, // session ended (close_reason has why)
        kSendNotHandshaked = -3, // no connected accepted socket yet
        kSendInvalidArg    = -4,
        kSendError         = -5, // srt-level error (see close_reason)
    };

    // `sink`/`sink_opaq` must outlive this object.
    explicit ReceiverCore(ReceiverCoreParams params, DatagramSink sink, void* sink_opaq);
    ~ReceiverCore();

    ReceiverCore(const ReceiverCore&)            = delete;
    ReceiverCore& operator=(const ReceiverCore&) = delete;

    void Configure(const ReceiverCoreParams& params);

    // ---- driver thread (transport owner) ----

    // Builds the listener (socket, options, transport attach, bind-less
    // listen) and spawns the accept thread. measured_rtt_ms (already
    // clamped to 1..30000, 0 = unmeasured) seeds SRTO_INITIAL_RTT
    // unless params_.initial_rtt_ms overrides it.
    // Returns false with *error set on a genuine setup failure (also
    // recorded as the close reason); a concurrent MarkLocalStop unwinds
    // silently and returns false with *error empty - the pending local
    // shutdown keeps its own reason.
    bool Start(int measured_rtt_ms, std::string* error);

    // Injects one incoming transport datagram into the srt receive
    // queue. No-op once the core is closed.
    void InjectDatagram(const std::uint8_t* data, std::size_t size);

    // Marks the end of the wire session (peer close, transport error).
    // `graceful` keeps the accepted socket open for the TSBPD drain
    // (Read keeps delivering; Finalize completes the deferred teardown).
    // Idempotent; an earlier setup-error reason wins over `reason`.
    void Shutdown(const std::string& reason, bool graceful);

    // ---- app thread ----

    // Waits until the SRT handshake completed (true), the core closed
    // (false, *why set), or timeout_ms elapsed (false, *why empty).
    bool WaitEstablished(int timeout_ms, std::string* why);

    // Waits until the core reached its terminal state. True when
    // closed, false on timeout.
    bool WaitClosed(int timeout_ms);

    // Delivers one SRT message (a TS chunk of at most the payload size)
    // into buf. len must be at least the configured payload size; see
    // ReadResult for the return contract. Bounded receive slices keep the
    // call responsive to a stop from another thread.
    int Read(char* buf, int len, int timeout_ms);

    // Sends len bytes out on the accepted socket: the call frames them
    // into messages of at most the negotiated payload size, each its own
    // srt message, and never carries a remainder across calls. Returns
    // the bytes accepted (a partial count when the deadline or the
    // session gave out midway - resume from buf + returned), 0 for
    // len == 0 (nothing is emitted), or a SendResult status. timeout_ms
    // bounds the whole call (0 = one send attempt).
    int SendMessage(const char* buf, int len, int timeout_ms);

    // Snapshots the accepted socket's stats (srt_bistats instantaneous,
    // no clear). False when there is no connected accepted socket.
    bool Stats(SRT_TRACEBSTATS* out);

    // Local stop marker: the next Shutdown (or Finalize) never defers
    // the teardown for the TSBPD drain - a local stop must stay prompt.
    void MarkLocalStop();

    // Idempotent terminal cleanup (sockets, accept thread, transport),
    // including any Shutdown-deferred drain teardown. Safe with the
    // driver still running; a no-op once closed.
    void Finalize();

    bool        IsClosed() const;
    bool        ClosedGracefully() const;
    std::string close_reason() const;

private:
    // ---- accept thread ----
    void AcceptThread(SRTSOCKET listener);

    // ---- srt threads: virtual transport send callback ----
    static int VtSendThunk(void* opaq, const char* data, int len, const struct sockaddr* dst, int dstlen);
    int        VtSend(const char* data, int len);

    // Closes a Shutdown-deferred accepted socket and releases its
    // transport (no-op when nothing was deferred).
    void CompleteDeferredTeardown();

    // Terminal transition: records the reason, wakes waiters, tears the
    // srt side down. `graceful` marks peer-clean ends. Idempotent; no
    // lock held on entry. An earlier setup-error reason wins.
    void FinalizeInternal(const std::string& reason, bool graceful);

    enum class State
    {
        kIdle,      // Start not called (or wire session not established yet)
        kListening, // srt listener up, accept thread parked
        kConnected, // SRT handshake complete (accepted socket live)
        kClosed,    // terminal
    };

    DatagramSink       sink_;
    void*              sink_opaq_;
    ReceiverCoreParams params_;

    // Fake address labeling the initiator for srt dispatch (mirrors M0's
    // caller label; the address never reaches a network, it only has to be
    // stable for the session).
    sockaddr_in peer_label_ = {};

    mutable std::mutex      mutex_;
    std::condition_variable cv_;
    State                   state_      = State::kIdle;
    bool                    local_stop_ = false;
    bool                    graceful_   = false;
    std::string             close_reason_; // first reason wins
    SRTSOCKET               listener_ = SRT_INVALID_SOCK;
    SRTSOCKET               accepted_ = SRT_INVALID_SOCK;
    SRTVirtualTransport*    vt_       = nullptr;
    std::thread             accept_thread_;

    // Effective payload size after the HSv5 negotiation (the accept
    // thread's readback, publisher.cpp's negotiated-payload lesson):
    // SendMessage frames to this, not params_.payload_size.
    int negotiated_payload_ = kSrtPayloadSize;

    // Drain window on a graceful peer end: Read may keep delivering the
    // accepted socket's buffered messages until this point (TSBPD latency
    // plus margin); afterwards the end is final.
    std::chrono::steady_clock::time_point drain_deadline_ = std::chrono::steady_clock::time_point::max();

    // Lock-free mirror of state_ == kClosed for the srt send callback.
    std::atomic<bool> closed_{false};
};

} // namespace websrt

#endif // WEBSRT_SRT_SESSION_RECEIVER_CORE_H
