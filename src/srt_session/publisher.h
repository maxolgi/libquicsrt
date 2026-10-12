// SRT-over-WT publisher session: the native analogue of the browser-side
// publisher (WebSRT web/src/worker.ts driving crates/srt-wasm). The
// gateway is the SRT initiator (caller) for EVERY session, publisher
// included: on WT session ready this session creates one forked-srt
// socket in virtual-transport mode, puts it into the listening state
// WITHOUT a bind (the fork synthesizes the label), and answers the HSv5
// Conclusion-first handshake the gateway floods every ~2 ms - the same
// listener half as the subscriber. The difference is the data plane
// direction: once accepted, the socket is the SENDER, and the gateway's
// SrtInitiator feeds whatever it releases (session.rs route_release_data)
// into the stream's broadcaster ring.
//
// Socket options are the subscriber's listener-half set (inherited by the
// accepted socket), matching srt-wasm's publisher-side ConnInitSettings:
//   SRTO_TRANSTYPE      SRTT_LIVE
//   SRTO_PAYLOADSIZE    params.payload_size (1128: 6 x 188 TS packets;
//                       HSv5 negotiates min(gateway, listener) - the
//                       chunker uses the negotiated readback)
//   SRTO_RCVLATENCY     params.tsbpd_latency_ms (TSBPD = max(both sides))
//   SRTO_PEERLATENCY    params.tsbpd_latency_ms
//   SRTO_SNDBUF/SRTO_RCVBUF  latency-scaled packets x 1472 bytes
//   SRTO_SKIP_INDUCTION 1 (listener accepts Conclusion-first)
//   SRTO_INITIAL_RTT    QUIC smoothed RTT clamped to 1..30000 ms
//
// Write path: the app thread hands over arbitrary-size buffers; TsChunker
// (below) frames them into <= payload-size, 188-aligned live messages and
// srt_sendmsg2 queues each into the send buffer. Backpressure is SRT's
// own: a full send buffer blocks the send until SRTO_SNDTIMEO (bounded
// by the Write budget), and the WT send callback never blocks - a full
// datagram queue surfaces as a failed VtSend, i.e. UDP-like loss that
// SRT retransmits.
//
// PACED-WRITE CAVEAT (the M0 lesson): an unthrottled blast fills the
// send buffer and the WT datagram queue faster than ACKs and the ~2 ms
// gateway ticker drain them, starving retransmit slots and dropping
// datagrams wholesale. Real publishers pace naturally at frame cadence
// (ffmpeg calls Write once per muxed packet cluster, the browser once
// per encoded frame); this session enforces NO pacing of its own, so a
// caller that dumps megabytes back-to-back gets the live-stream behavior
// SRT gives: blocking on the send buffer, then too-late drops.
//
// Stream end: End() flushes the chunker tail, waits until every sent
// message is ACKed (empty send buffer), then closes the accepted socket
// FIRST - srt_close emits the SRT shutdown through the still-open WT
// transport, the gateway's initiator drains its TSBPD-held tail into the
// ring (srt-protocol status.rs: Shutdown -> Drain releases until the
// receive buffer is flushed), and only then does the gateway close the
// WT session. Closing the WT before that would drop the receiver-held
// tail: the gateway prunes the session entry ~2 ms after the WT closes,
// TSBPD release notwithstanding. If the gateway does not close within
// the budget, a fallback forces the WT close (tail loss accepted).
//
// THREADS AND TEARDOWN ORDER: the subscriber's four domains, unchanged
// (loop thread = WTClientListener + engine driving; srt multiplexer
// threads = VtSend; a short-lived accept thread; the app thread =
// Start/Write/End/Stats/Stop). The lifetime rules are the subscriber's:
// state under mutex_, cv_ for transitions, OnDatagram injects under the
// mutex so Finalize can park it, and the shutdown order is state ->
// kClosed + vt_ = null under the mutex, then OUTSIDE it srt_close
// (accepted first, then listener - wakes a blocked accept), join the
// accept thread, and release the transport handle last (srt_close sends
// its shutdown through the send callback). One DELTA: there is no
// drain-deferred teardown - the subscriber keeps a gracefully-ended
// accepted socket open to drain TSBPD-held RECEIVED data, but a publisher
// receives only ACK/NAK, so the teardown is immediate.

#ifndef WEBSRT_SRT_SESSION_PUBLISHER_H
#define WEBSRT_SRT_SESSION_PUBLISHER_H

#include <arpa/inet.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "wt/client.h"

// The fork's gated API (build with SRT_ENABLE_VIRTUAL_TRANSPORT defined).
#include "srt.h"

namespace websrt
{

struct PublisherSessionParams
{
    int payload_size     = 1128; // WebSRT constant: 6 x 188-byte TS packets
    int tsbpd_latency_ms = 120;  // srt-wasm publisher default; TSBPD = max(sender, receiver)
    // 0 = seed from the WT layer's smoothed RTT at session-ready time.
    int initial_rtt_ms = 0;
};

// MPEG-TS packet size on the wire this session frames (never split).
constexpr int kTsPacketSize = 188;

// Splits a TS byte stream into SRT live messages: every message is a
// whole number of 188-byte TS packets, at most payload_size long, and a
// sub-188 remainder carries over between Feed calls (so a 4096-byte Write
// and a 1316-byte one produce the same 1128-byte framing). Pure buffering
// logic - no sockets, no locking - which is what the unit tests drive.
class TsChunker
{
public:
    explicit TsChunker(int payload_size);

    // Appends bytes to the pending stream (accepted by the caller).
    void Feed(const void* data, std::size_t len);

    // Bytes currently buffered (fed, not yet emitted as messages).
    [[nodiscard]] std::size_t pending() const;

    // Length of the next message to emit: the largest multiple of 188
    // that fits both the pending stream and the payload size; 0 when no
    // complete TS packet is buffered.
    [[nodiscard]] int NextChunkLen() const;

    // Front of the pending stream (the bytes of the next message).
    [[nodiscard]] const std::uint8_t* data() const;

    // Drops the first n bytes - a message just handed to the sender.
    void Consume(int n);

    // Rolls the pending stream back to `size` bytes (undoes a Feed;
    // `size` must be a previously observed pending()).
    void TruncateTo(std::size_t size);

private:
    int                       payload_size_ = 1128;
    std::vector<std::uint8_t> pending_;
};

class PublisherSession final : public WTClientListener
{
public:
    // Write outcomes; positive values carry the byte count accepted from
    // the caller's buffer, so Write() returns plain int and these are its
    // documented constants. They mirror the public Publisher's values
    // (include/websrt/publisher.hpp).
    enum WriteResult
    {
        kWriteNotStarted = -1,
        kWriteClosed     = -2, // session ended (peer or gateway); close_reason has why
        kWriteInvalidArg = -3,
        kWriteTimeout    = -4, // no message accepted within timeout_ms
        kWriteError      = -5, // srt-level error (see close_reason)
    };

    // `client` must outlive this object. Two-phase wiring exactly like
    // SubscriberSession: construct, AttachClient once the WTClient exists,
    // Configure before the session goes live.
    explicit PublisherSession(PublisherSessionParams params);
    ~PublisherSession();

    PublisherSession(const PublisherSession&)            = delete;
    PublisherSession& operator=(const PublisherSession&) = delete;

    void AttachClient(WTClient& client);
    void Configure(const PublisherSessionParams& params);

    // ---- app thread ----

    // Waits until the SRT handshake completed (true), the session closed
    // (false, *why set), or timeout_ms elapsed (false, *why empty).
    bool WaitEstablished(int timeout_ms, std::string* why);

    // Waits until the session reached its terminal state.
    bool WaitClosed(int timeout_ms);

    // Frames data into TS-aligned live messages and queues each into the
    // send buffer. Returns the number of bytes ACCEPTED from `data`: len
    // when everything went out (a sub-188 tail stays buffered for the
    // next Write or End), less when the budget or the transport gave out
    // midway - the caller resumes from data + returned. Zero progress
    // within timeout_ms is kWriteTimeout. timeout_ms bounds the whole
    // call (0 = poll).
    int Write(const void* data, int len, int timeout_ms);

    // Graceful stream end: flushes the chunker tail, waits for the send
    // buffer to fully ACK, closes the accepted socket (SRT shutdown rides
    // the open WT session so the gateway drains its TSBPD-held tail into
    // the ring), and waits for the gateway's WT close. False (reason in
    // close_reason) on timeout - the caller falls back to Stop().
    bool End(int timeout_ms);

    // Snapshots the accepted socket's stats (srt_bstats, no clear).
    // False when there is no connected accepted socket.
    bool Stats(SRT_TRACEBSTATS* out);

    // Local WT-level close (abrupt for the stream: the tail is dropped).
    // Used by End's fallback and the public Stop; idempotent, any thread.
    void RequestClose();

    // Idempotent terminal cleanup (sockets, accept thread, transport).
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
    bool StartListener(std::string* error);

    // Terminal transition: records the reason, wakes waiters, tears the
    // srt side down. `graceful` marks locally-requested or peer-clean
    // ends. Idempotent; no lock held on entry. An earlier setup error
    // reason wins over `reason`.
    void Finalize(const std::string& reason, bool graceful);

    // Sends one message; >= 0 is the srt result, and *error_code carries
    // srt_getlasterror for the failure paths.
    int SendOne(const std::uint8_t* buf, int len, int timeout_ms);

    // Waits (bounded, polling) until the accepted socket's send buffer is
    // fully ACKed. True on drain, false on timeout or a lost socket.
    bool WaitSendBufferDrained(int timeout_ms);

    enum class State
    {
        kIdle,      // WT session not established yet
        kListening, // srt listener up, accept thread parked
        kConnected, // SRT handshake complete (accepted socket live)
        kClosed,    // terminal
    };

    WTClient*              client_ = nullptr; // set by AttachClient, pre-use
    PublisherSessionParams params_;

    // Fake address labeling the gateway for srt dispatch (the subscriber's
    // caller label; the address never reaches a network).
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

    // App-thread-only (Write/End run on the controlling thread): the
    // negotiated payload size (read back after accept) and the framing.
    TsChunker chunker_{1128};

    // Lock-free mirror of state_ == kClosed for the srt send callback.
    std::atomic<bool> closed_{false};
};

} // namespace websrt

#endif // WEBSRT_SRT_SESSION_PUBLISHER_H
