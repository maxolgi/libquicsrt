// Publisher: the libquicsrt client for feeding a QuicSRT stream (SRT over
// WebTransport), the native counterpart of the browser-side publisher
// and the backend of the FFmpeg quicsrt:// output.
//
// One Publisher instance performs exactly one session. Start() blocks
// until the stream is fully established (WebTransport CONNECT accepted
// and the SRT handshake complete) or fails; then Write() pushes the
// stream's MPEG-TS bytes (any chunk sizes - the session frames them
// into 188-aligned SRT messages of at most the negotiated payload size,
// default 1128); End() finishes the stream gracefully; Stop() (or the
// destructor) tears everything down at any time. To publish again,
// construct a fresh Publisher.
//
// Write semantics: the return value is the number of bytes ACCEPTED
// from the caller's buffer. len means everything went out (a sub-188
// remainder stays buffered inside and prefixes the next Write). A
// partial positive return means the connect timeout budget or the
// transport gave out midway: the caller resumes from
// data + returned (the session holds nothing of the un-accepted tail).
// Negative returns are the WriteResult error codes. Write never paces
// the stream by itself: publishers naturally call it at frame cadence,
// and an unthrottled megabyte blast just fills SRT's send buffer until
// timeout_ms blocks it (the M0 starve-the-retransmits lesson).
//
// Stream end: End() flushes the buffered tail, waits until the gateway
// ACKed every message, and closes the SRT connection through the still
// open WebTransport session - the gateway then drains its receive-side
// TSBPD window into the broadcast ring and closes the session, which is
// what ends the broadcast and gives every subscriber a clean EOF. The
// gateway releases the stream name when that happens, so a new Start
// for the same name succeeds again. Stop() is the abrupt variant: the
// not-yet-released tail is dropped (live-stream semantics).
//
// First-wins occupancy: while this (or any) live source holds the
// stream name, the gateway answers a challenger's CONNECT with HTTP 403
// and Start surfaces kPublishRejected.
//
// Threading: Start/Write/End/Stats/LastError/Stop belong to one
// controlling thread (Write blocks in SRT's send buffer under
// backpressure, bounded by its timeout). Stop is safe to call from
// another thread and unblocks an in-flight Write within a fraction of a
// second. No method throws.
//
// Usage sketch:
//
//     quicsrt::PublisherConfig cfg;
//     cfg.host = "127.0.0.1"; cfg.port = 4433; cfg.stream_name = "demo";
//     cfg.cert_sha256_hex = "<64 hex chars from cert-hash.js>";
//     if (pub.Start(cfg) != quicsrt::kPublishOk) { /* pub.LastError() */ }
//     for (each muxed chunk) {
//         int n = pub.Write(chunk, len, 1000);
//         if (n < 0) break;                 // error; LastError() has why
//         chunk += n; len -= n;             // resume on partial accept
//     }
//     pub.End(5000);                        // graceful stream end
//

#ifndef QUICSRT_PUBLISHER_HPP
#define QUICSRT_PUBLISHER_HPP

#include <cstddef>
#include <memory>
#include <string>

#include "quicsrt/config.hpp"
#include "quicsrt/stats.hpp"

namespace quicsrt
{

// Outcome of Publisher::Start().
enum PublisherStartStatus
{
    kPublishOk = 0,         // stream established; Write may push data
    kPublishInvalidConfig,  // config.validate() failed; LastError() has the reason
    kPublishAlreadyStarted, // Start called twice; a Publisher is single-shot
    kPublishInvalidTarget,  // the target was rejected (e.g. DNS name did not resolve to an IPv4 address)
    kPublishRejected,       // the gateway refused the session: HTTP 403 while the stream name is held by a live source
                            // (first-wins) or 429 on limits; 5xx is kPublishConnectFailed, not a refusal
    kPublishConnectFailed,  // transport-level failure (TLS, network, engine) or a non-refusal HTTP status (e.g. 5xx)
    kPublishTimedOut,       // connect_timeout_ms elapsed; the session was torn down
    kPublishInternal,       // unexpected library-internal failure
};

// Negative return values of Publisher::Write(); positive values are the
// byte count accepted from the caller's buffer (see the class comment
// for the partial-accept contract).
enum WriteResult
{
    kWriteNotStarted = -1, // Start was never called (or returned != kPublishOk)
    kWriteClosed     = -2, // session ended (stream end, gateway, or local Stop); LastError() has the reason
    kWriteInvalidArg = -3, // null buffer or non-positive length
    kWriteTimeout    = -4, // nothing accepted within timeout_ms; safe to call again
    kWriteError      = -5, // SRT-level error (see LastError())
};

class Publisher final
{
public:
    Publisher();
    ~Publisher(); // Stop() if a session is still alive

    Publisher(const Publisher&)            = delete;
    Publisher& operator=(const Publisher&) = delete;
    Publisher(Publisher&&)                 = delete;
    Publisher& operator=(Publisher&&)      = delete;

    // Starts and fully establishes the session: spawns the event loop,
    // opens the WebTransport session to <host>:<port> with the path
    // /wt?publish=<name>, answers the SRT handshake (the gateway
    // initiates as soon as the CONNECT is accepted), and returns once
    // the stream is ready to Write, the gateway refused it, or
    // connect_timeout_ms elapsed. On any non-ok result the session is
    // torn down and LastError() describes what happened.
    PublisherStartStatus Start(const PublisherConfig& config);

    // Pushes len bytes of MPEG-TS into the stream. The session frames
    // them into 188-aligned live messages (never splitting a TS
    // packet); a sub-188 remainder carries over to the next call.
    // timeout_ms bounds the whole call (0 = poll). See the class
    // comment for the accepted-bytes contract.
    int Write(const void* data, std::size_t len, int timeout_ms);

    // Ends the stream gracefully: flushes the buffered tail, waits for
    // every message to be ACKed, and shuts the SRT connection down
    // through the open WebTransport session so the gateway ends the
    // broadcast (subscribers see EOF) and frees the stream name.
    // timeout_ms bounds the whole sequence; on timeout the caller falls
    // back to Stop() (which drops the not-yet-released tail). Idempotent
    // once ended.
    bool End(int timeout_ms);

    // Snapshots the SRT statistics of the live connection. Valid while
    // streaming; after the connection ends the totals are no longer
    // readable (kStatsNotConnected), so poll periodically if the final
    // numbers matter.
    StatsStatus Stats(PublisherStats* out);

    // Tears the session down (WT close, SRT sockets closed, threads
    // joined). Idempotent; unblocks an in-flight Write within a
    // fraction of a second. The Publisher cannot be restarted.
    void Stop();

    // Human-readable description of the most recent failure; empty when
    // everything succeeded. Populated by Start failures, Write error
    // paths, and a failed End.
    std::string LastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace quicsrt

#endif // QUICSRT_PUBLISHER_HPP
