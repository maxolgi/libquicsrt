// Subscriber: the libwebsrt client for viewing a WebSRT stream
// (SRT over WebTransport), the native counterpart of the browser player.
//
// One Subscriber instance performs exactly one session. Start() blocks
// until the stream is fully established (WebTransport CONNECT accepted
// and the SRT handshake complete) or fails; then Read() delivers the
// stream's MPEG-TS bytes one SRT message at a time. Stop() (or the
// destructor) tears everything down. To subscribe again, construct a
// fresh Subscriber.
//
// Threading: Start/Stats/LastError/Stop belong to one controlling
// thread. Read may run on the same or a dedicated thread; Stop is safe
// to call from another thread and unblocks an in-flight Read within a
// fraction of a second. No method throws.
//
// Usage sketch:
//
//     websrt::SubscriberConfig cfg;
//     cfg.host = "127.0.0.1"; cfg.port = 4433; cfg.stream_name = "demo";
//     cfg.cert_sha256_hex = "<64 hex chars from cert-hash.js>";
//     if (sub.Start(cfg) != websrt::kStartOk) { /* sub.LastError() */ }
//     char buf[1128];
//     for (;;) {
//         int n = sub.Read(buf, sizeof buf, 1000);
//         if (n <= 0) break;            // end, timeout-policy, or error
//         fwrite(buf, 1, n, out);
//     }

#ifndef WEBSRT_SUBSCRIBER_HPP
#define WEBSRT_SUBSCRIBER_HPP

#include <cstddef>
#include <memory>
#include <string>

#include "websrt/config.hpp"
#include "websrt/stats.hpp"

namespace websrt
{

// Outcome of Subscriber::Start().
enum StartStatus
{
    kStartOk = 0,         // stream established; Read delivers data
    kStartInvalidConfig,  // config.validate() failed; LastError() has the reason
    kStartAlreadyStarted, // Start called twice; a Subscriber is single-shot
    kStartInvalidTarget,  // the transport rejected the target (e.g. host not an IPv4 literal)
    kStartRejected,       // the gateway refused the session (unknown stream: HTTP 404)
    kStartConnectFailed,  // transport-level failure (TLS, network, engine)
    kStartTimedOut,       // connect_timeout_ms elapsed; the session was torn down
    kStartInternal,       // unexpected library-internal failure
};

// Outcome of Subscriber::Stats() lives in websrt/stats.hpp (shared with
// the Publisher).

// Return values of Subscriber::Read(). Positive values are the number of
// bytes delivered into the caller's buffer (one SRT message - a
// 188-aligned TS chunk of at most the configured payload size).
enum ReadResult
{
    kReadEnd        = 0,  // stream ended gracefully (peer or local Stop); sticky
    kReadTimeout    = -1, // no message within timeout_ms; safe to call again
    kReadClosed     = -2, // session ended with an error; LastError() has the reason
    kReadNotStarted = -3, // Start was never called (or returned != kStartOk)
    kReadInvalidArg = -4, // null buffer or non-positive length
    kReadError      = -5, // SRT-level error (buffer too small for the message, socket error)
};

class Subscriber final
{
public:
    Subscriber();
    ~Subscriber(); // Stop() if a session is still alive

    Subscriber(const Subscriber&)            = delete;
    Subscriber& operator=(const Subscriber&) = delete;
    Subscriber(Subscriber&&)                 = delete;
    Subscriber& operator=(Subscriber&&)      = delete;

    // Starts and fully establishes the session: spawns the event loop,
    // opens the WebTransport session to <host>:<port> with the path
    // /wt?stream=<name>, answers the SRT handshake, and returns once the
    // stream is ready (kStartOk), the gateway or transport refused it, or
    // connect_timeout_ms elapsed. On any non-ok result the session is
    // torn down and LastError() describes what happened.
    StartStatus Start(const SubscriberConfig& config);

    // Delivers one SRT message into buf. len should be at least the
    // configured payload_size; a smaller buffer than the next message
    // surfaces kReadError. timeout_ms bounds the whole call (0 = poll);
    // kReadTimeout means "nothing yet, call again". A Read that observes
    // the stream end returns kReadEnd; the definitive terminal status
    // (kReadEnd vs kReadClosed) is reflected by the following call.
    int Read(void* buf, std::size_t len, int timeout_ms);

    // Snapshots the SRT statistics of the live connection. Valid while
    // streaming; after the connection ends the totals are no longer
    // readable (kStatsNotConnected), so poll periodically if the final
    // numbers matter.
    StatsStatus Stats(SubscriberStats* out);

    // Tears the session down (graceful WebTransport close, SRT sockets
    // closed, threads joined). Idempotent; unblocks an in-flight Read
    // within a fraction of a second. The Subscriber cannot be restarted.
    void Stop();

    // Human-readable description of the most recent failure; empty when
    // everything succeeded. Populated by Start failures, kReadClosed, and
    // kReadError paths.
    std::string LastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace websrt

#endif // WEBSRT_SUBSCRIBER_HPP
