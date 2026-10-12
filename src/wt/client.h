// WebTransport client over lsquic: one engine and one extended-CONNECT
// session toward a WebSRT gateway, with WT datagrams both ways. The
// proven spike disciplines live here (tests/wt_echo.cpp at the spike
// commit): LSENG_HTTP + QUIC v1 + es_datagrams engine recipe, raw lsxpack
// extended-CONNECT headers followed by an explicit flush, cert pinning by
// SHA-256-of-DER, HTTP/1.x-style response parsing with the built-in header
// interface, capsule skipping with CLOSE detection, and quarter-stream-id
// context-prefixed datagram framing.

#ifndef WEBSRT_WT_CLIENT_H
#define WEBSRT_WT_CLIENT_H

#include <netinet/in.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <string>
#include <vector>

#include "core/event_loop.h"

struct lsquic_engine;
struct lsquic_conn;
struct lsquic_stream;
typedef struct lsquic_engine lsquic_engine_t;
typedef struct lsquic_conn   lsquic_conn_t;
typedef struct lsquic_stream lsquic_stream_t;

namespace websrt
{ // Consumer callbacks. All fire on the event-loop thread and may call back
// into WTClient.
class WTClientListener
{
public:
    virtual ~WTClientListener() = default;

    // Extended CONNECT accepted (HTTP 200): the session stream is open and
    // SendDatagram may be used.
    virtual void OnSessionReady() {}

    // Terminal event, fired at most once per session: TLS handshake failure,
    // a non-200 CONNECT response, or the connection closing (local Close
    // included). After it returns, the engine is destroyed and the client is
    // inert. Not fired for destructor-driven teardown.
    virtual void OnSessionClosed(const std::string& /*reason*/) {}

    // A WT datagram payload arrived; the quarter-stream-id context prefix is
    // already stripped. Malformed frames are dropped silently.
    virtual void OnDatagram(const std::uint8_t* /*data*/, std::size_t /*size*/) {}

    // The session stream reached a terminal event: a CLOSE capsule arrived,
    // the stream hit FIN, or the stream was closed. Capsule payloads are
    // consumed internally; there is nothing else to read from the stream.
    virtual void OnStreamReadable() {}
};

class WTClient : private EngineHost
{
public:
    struct Target
    {
        std::string   host; // IPv4 dotted quad
        std::uint16_t port = 0;
        std::string   path;          // with query, e.g. "/wt?stream=demo"
        std::string   cert_hash_hex; // expected SHA-256 of the cert DER, 64 hex chars
    };

    // Largest payload SendDatagram accepts: it must fit next to the context
    // prefix inside a datagram frame on a minimum-MTU packet (SRT's 1128-byte
    // payload does). The transport's actual ceiling is learned at runtime;
    // queued datagrams that exceed it are dropped whole and counted.
    static constexpr std::size_t kMaxDatagramSize    = 1184;
    static constexpr std::size_t kMaxQueuedDatagrams = 256;

    // `listener` must outlive the client. Supported teardown orderings:
    // Close() then destroy after OnSessionClosed (the loop may keep
    // running), or stop the loop and destroy in any order afterwards. Never
    // destroy from inside a listener callback; never race the destructor
    // with EventLoop::Stop.
    WTClient(EventLoop& loop, WTClientListener& listener);
    ~WTClient();

    WTClient(const WTClient&)            = delete;
    WTClient& operator=(const WTClient&) = delete;

    // Thread-safe. Starts the TLS+QUIC handshake and the extended CONNECT.
    // Returns false when the client is not idle or the target does not parse
    // (host not an IPv4 literal, zero port, empty path, hash not 64 hex
    // chars). Runtime failures (socket, engine) surface as OnSessionClosed.
    bool Connect(const Target& target);

    // Thread-safe. Queues one WT datagram; the context prefix is added
    // internally. Returns false when there is no open session or the payload
    // is empty or over kMaxDatagramSize. A full queue drops the datagram and
    // counts it in DatagramsDropped().
    bool SendDatagram(const void* data, std::size_t size);

    // Thread-safe. Graceful shutdown: sends CONNECTION_CLOSE and tears the
    // engine down once the connection finishes closing (OnSessionClosed
    // then fires). No-op when idle or already closed.
    void Close();

    // Thread-safe evidence getters.
    int                       HttpStatus() const;  // -1 until the CONNECT response
    std::chrono::microseconds SmoothedRtt() const; // 0 until measured
    int                       DatagramsReceived() const;
    int                       DatagramsWritten() const;
    int                       DatagramsDropped() const;
    int                       UdpPacketsReceived() const;

private:
    enum class State
    {
        kIdle,
        kConnecting,
        kConnected,
        kClosing,
        kClosed,
    };

    // lsquic callback shims (client.cpp) bridge the C callbacks to the
    // members below with `this` carried as the engine context.
    friend struct WtClientCallbacks;

    // EngineHost: loop-thread engine driving.
    bool NextTick(std::chrono::microseconds& diff) override;
    void Pump() override;

    // Loop-thread internals.
    void ConnectOnLoop(const Target& target);
    void EnqueueDatagram(std::vector<std::uint8_t> payload);
    void CloseOnLoop();
    // Destroys the engine outside any engine callback; idempotent. `notify`
    // fires OnSessionClosed when this call is what closes the session.
    void    Teardown(const std::string& reason, bool notify);
    void    ScheduleTeardown(const std::string& reason);
    void    HandleUdpReadable();
    void    SendConnectHeaders(lsquic_stream_t* stream);
    void    ReadSessionStream(lsquic_stream_t* stream);
    void    OnDatagramFromEngine(const void* buf, std::size_t size);
    ssize_t OnDgWriteFromEngine(void* buf, std::size_t size);
    // Returns the bytes consumed by the response header block, 0 when the
    // terminator has not arrived yet; sets http_status_.
    std::size_t ParseResponseHeaders();
    void        ParseCapsules(const std::uint8_t* data, std::size_t size);

    EventLoop&        loop_;
    WTClientListener& listener_;

    // Target, staged by Connect and consumed on the loop thread.
    std::string   authority_;
    std::string   path_;
    std::uint8_t  expected_hash_[32] = {};
    std::uint16_t port_              = 0;

    // Transport state: loop thread only (the loop is stopped or the thread
    // joined before the destructor touches these inline).
    lsquic_engine* engine_        = nullptr;
    lsquic_conn*   conn_          = nullptr;
    int            fd_            = -1;
    bool           fd_registered_ = false;
    sockaddr_in    local_sa_{};
    sockaddr_in    peer_sa_{};

    std::uint64_t             stream_id_       = UINT64_MAX;
    bool                      headers_sent_    = false;
    bool                      headers_done_    = false;
    bool                      stream_terminal_ = false;
    std::vector<std::uint8_t> session_buf_; // response headers remainder + capsules

    std::deque<std::vector<std::uint8_t>> send_queue_;
    std::size_t                           datagram_write_size_ = 0;

    // State and evidence. state_ transitions on the loop thread; the other
    // atomics are written on the loop thread and read from anywhere.
    std::atomic<State>         state_{State::kIdle};
    std::atomic<int>           http_status_{-1};
    std::atomic<std::uint64_t> rtt_us_{0};
    std::atomic<int>           datagrams_received_{0};
    std::atomic<int>           datagrams_written_{0};
    std::atomic<int>           datagrams_dropped_{0};
    std::atomic<int>           udp_packets_received_{0};

    // Resolved by the destructor's posted teardown when the loop is still
    // running; makes mid-session destruction safe outside callbacks.
    std::promise<void> teardown_done_;
};

// Routes lsquic's own diagnostic logging to stderr; `level` as in
// lsquic_set_log_level (e.g. "debug"). Process-wide; call before Connect.
void SetLsquicLogLevel(const char* level);

} // namespace websrt

#endif // WEBSRT_WT_CLIENT_H
