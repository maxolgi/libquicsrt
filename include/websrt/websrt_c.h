// websrt_c: the C ABI over libwebsrt (Haivision srt.h spirit: opaque
// handles, integer status codes, no exceptions across the boundary).
//
// One handle performs exactly one session, mirroring the C++ façades
// (publisher.hpp / subscriber.hpp): start it, pump data, end/stop it,
// destroy it. The semantics are those of the C++ classes; this header
// only restates them in C-callable form:
//
//   - websrt_*_start blocks until the session is fully established
//     (WebTransport CONNECT accepted + SRT handshake complete), fails,
//     or the connect timeout elapses. Parameters <= 0 pick the library
//     defaults (port 443, latency 120 ms, connect timeout 10 s).
//   - websrt_publisher_write returns the number of bytes ACCEPTED
//     (len = all went out; a partial positive means the timeout budget
//     gave out midway - resume from data + returned). Negative returns
//     are the WEBSRT_WRITE_* codes.
//   - websrt_subscriber_read delivers one SRT message (a 188-aligned
//     TS chunk of at most 1128 bytes - the buffer should be at least
//     that large) or a WEBSRT_READ_* code. WEBSRT_READ_TIMEOUT is
//     retryable; WEBSRT_READ_END is the sticky graceful end.
//   - websrt_publisher_end flushes and ends the stream gracefully
//     (subscribers see EOF, the stream name frees); any failure falls
//     back to websrt_publisher_stop, which is always safe to call.
//   - Stats are a single formatted key=value line into a caller
//     buffer (always NUL-terminated, truncated to fit); the counters
//     follow SRT's byte convention (payload + per-packet headers).
//   - Threading: the data-pump calls belong to one controlling
//     thread; stop (and destroy) may be called from another thread
//     and unblock an in-flight write/read within a fraction of a
//     second. Nothing throws.
//
// All functions accept NULL handles/buffers gracefully (WEBSRT_EINVAL
// / NULL). Error strings live on the handle and stay valid until the
// next call on that handle (or its destruction).

#ifndef WEBSRT_WEBSRT_C_H
#define WEBSRT_WEBSRT_C_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Library version string ("0.1.0"). Never NULL.
const char* websrt_version(void);

// Status codes returned by the non-pump calls. Negative, srt.h-style.
typedef enum websrt_status
{
    WEBSRT_OK        = 0,   // success
    WEBSRT_EINVAL    = -1,  // invalid argument (NULL handle/buffer, bad params)
    WEBSRT_ENOMEM    = -2,  // allocation failure
    WEBSRT_ECONFIG   = -3,  // configuration failed validation (last_error has the constraint)
    WEBSRT_ESTATE    = -4,  // wrong session state (double start, stats before start)
    WEBSRT_ETARGET   = -5,  // target rejected (e.g. DNS name did not resolve to an IPv4 address)
    WEBSRT_EREJECTED = -6,  // gateway refused: publish 403 (name in use, first-wins) / subscribe 404 (no such stream) / 429 (limits)
    WEBSRT_ECONNECT  = -7,  // transport-level failure (TLS, network, engine) or a non-refusal CONNECT status (e.g. 5xx)
    WEBSRT_ETIMEOUT  = -8,  // connect timeout elapsed; the session was torn down
    WEBSRT_ECLOSED   = -9,  // session already ended
    WEBSRT_EINTERNAL = -10, // unexpected library-internal failure
} websrt_status_t;

// websrt_publisher_write return values. Positive: bytes accepted from
// the caller's buffer (see the partial-accept contract above).
#define WEBSRT_WRITE_NOT_STARTED (-1) // start never succeeded
#define WEBSRT_WRITE_CLOSED      (-2) // session ended (graceful end, gateway, or local stop)
#define WEBSRT_WRITE_INVALID     (-3) // NULL data or non-positive length
#define WEBSRT_WRITE_TIMEOUT     (-4) // nothing more accepted within timeout_ms; safe to call again
#define WEBSRT_WRITE_ERROR       (-5) // SRT-level error (last_error has the reason)

// websrt_subscriber_read return values. Positive: bytes delivered (one
// SRT message, at most the negotiated payload size - 1128 for WebSRT).
#define WEBSRT_READ_END         0  // stream ended gracefully; sticky
#define WEBSRT_READ_TIMEOUT     (-1) // no message within timeout_ms; retryable
#define WEBSRT_READ_CLOSED      (-2) // session ended with an error
#define WEBSRT_READ_NOT_STARTED (-3) // start never succeeded
#define WEBSRT_READ_INVALID     (-4) // NULL buffer or non-positive length
#define WEBSRT_READ_ERROR       (-5) // SRT-level error (buffer too small for the message, socket error)

typedef struct websrt_publisher   websrt_publisher_t;
typedef struct websrt_subscriber websrt_subscriber_t;

// ---- Publisher ----

// Returns a fresh single-shot publisher handle, or NULL on allocation
// failure.
websrt_publisher_t* websrt_publisher_create(void);

// Destroys the handle (graceful teardown of any live session). NULL is
// a no-op.
void websrt_publisher_destroy(websrt_publisher_t* pub);

// Starts the session toward host:port with the given stream name (the
// gateway path becomes /wt?publish=<stream>). cert_hash_hex is the
// SHA-256 of the gateway certificate's DER, 64 hex characters, or NULL
// for normal PKI validation. latency_ms seeds TSBPD latency (the
// effective delivery delay is max(sender, receiver)); timeout_ms
// bounds the whole connect. Values <= 0 select the library defaults.
websrt_status_t websrt_publisher_start(
    websrt_publisher_t* pub, const char* host, int port, const char* stream,
    const char* cert_hash_hex, int latency_ms, int timeout_ms);

// Pushes len MPEG-TS bytes; the session frames them into 188-aligned
// live messages. timeout_ms bounds the whole call (0 = poll).
int websrt_publisher_write(websrt_publisher_t* pub, const void* data, size_t len, int timeout_ms);

// Ends the stream gracefully (flush + ACK drain + shutdown through the
// open WebTransport session; subscribers see EOF, the name frees).
// timeout_ms bounds the sequence; on any non-OK result the caller falls
// back to websrt_publisher_stop. Idempotent once ended.
websrt_status_t websrt_publisher_end(websrt_publisher_t* pub, int timeout_ms);

// Tears the session down at any time (drops the not-yet-sent tail).
// Idempotent; safe from another thread; unblocks an in-flight write.
void websrt_publisher_stop(websrt_publisher_t* pub);

// Formats the live SRT statistics as one key=value line into buf
// (NUL-terminated, truncated to buflen). WEBSRT_ESTATE when there is
// no live connection.
websrt_status_t websrt_publisher_stats(websrt_publisher_t* pub, char* buf, size_t buflen);

// Human-readable description of the most recent failure ("" when
// everything succeeded). Never NULL for a non-NULL handle.
const char* websrt_publisher_last_error(websrt_publisher_t* pub);

// ---- Subscriber ----

websrt_subscriber_t* websrt_subscriber_create(void);

void websrt_subscriber_destroy(websrt_subscriber_t* sub);

// Starts the session toward host:port with the given stream name (the
// gateway path becomes /wt?stream=<stream>). Parameter semantics match
// websrt_publisher_start (cert_hash_hex NULL = PKI).
websrt_status_t websrt_subscriber_start(
    websrt_subscriber_t* sub, const char* host, int port, const char* stream,
    const char* cert_hash_hex, int latency_ms, int timeout_ms);

// Delivers one SRT message into buf (len should be >= the negotiated
// payload size, 1128 for WebSRT). timeout_ms bounds the whole call
// (0 = poll).
int websrt_subscriber_read(websrt_subscriber_t* sub, void* buf, size_t len, int timeout_ms);

// Formats the live SRT statistics as one key=value line into buf.
// WEBSRT_ESTATE when there is no live connection.
websrt_status_t websrt_subscriber_stats(websrt_subscriber_t* sub, char* buf, size_t buflen);

// Tears the session down (graceful WebTransport close). Idempotent;
// safe from another thread; unblocks an in-flight read.
void websrt_subscriber_stop(websrt_subscriber_t* sub);

// Human-readable description of the most recent failure. Never NULL
// for a non-NULL handle.
const char* websrt_subscriber_last_error(websrt_subscriber_t* sub);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // WEBSRT_WEBSRT_C_H
