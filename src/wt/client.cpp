#include "wt/capsule.h"
#include "wt/client.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <arpa/inet.h>

#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

#include <lsxpack_header.h>
#include <lsquic.h>

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>

namespace websrt
{

namespace
{

constexpr std::size_t kRecvBufSize = 16384;
constexpr char        kUserAgent[] = "websrt-cpp";

void Log(const char* fmt, ...)
{
    std::fputs("wt-client: ", stderr);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

std::string HexStr(const std::uint8_t* p, std::size_t n)
{
    static const char kHex[] = "0123456789abcdef";
    std::string       s;
    s.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i)
    {
        s += kHex[p[i] >> 4];
        s += kHex[p[i] & 0xf];
    }
    return s;
}

bool ParseHashHex(const std::string& hex, std::uint8_t out[32])
{
    constexpr std::size_t kSha256HexLen = 64;
    auto                  nibble        = [](char c) -> int
    {
        if (c >= '0' && c <= '9')
        {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f')
        {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F')
        {
            return c - 'A' + 10;
        }
        return -1;
    };
    if (hex.size() != kSha256HexLen)
    {
        return false;
    }
    for (std::size_t i = 0; i < 32; ++i)
    {
        const int hi = nibble(hex[2 * i]);
        const int lo = nibble(hex[(2 * i) + 1]);
        if (hi < 0 || lo < 0)
        {
            return false;
        }
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

int LoggerLogBuf(void* /*ctx*/, const char* buf, std::size_t len)
{
    std::fwrite(buf, 1, len, stderr);
    return static_cast<int>(len);
}

} // namespace

void SetLsquicLogLevel(const char* level)
{
    static const struct lsquic_logger_if kLogger = {.log_buf = LoggerLogBuf};
    lsquic_logger_init(&kLogger, nullptr, LLTS_NONE);
    lsquic_set_log_level(level);
}

// lsquic callback shims. `this` rides as the stream-if ctx (on_new_conn,
// on_new_stream), the per-connection ctx returned from OnNewConn
// (on_conn_closed / on_hsk_done / on_datagram / on_dg_write have no ctx
// parameter), and the packets-out / verify ctx.
struct WtClientCallbacks
{
    static const struct lsquic_stream_if kStreamIf;

    static WTClient* FromConn(lsquic_conn_t* conn) { return reinterpret_cast<WTClient*>(lsquic_conn_get_ctx(conn)); }

    static lsquic_conn_ctx_t* OnNewConn(void* ctx, lsquic_conn_t* conn)
    {
        auto* c  = static_cast<WTClient*>(ctx);
        c->conn_ = conn;
        return reinterpret_cast<lsquic_conn_ctx_t*>(c);
    }

    static void OnConnClosed(lsquic_conn_t* conn)
    {
        auto* c = FromConn(conn);
        if (c == nullptr)
        {
            return;
        }
        // The connection object is gone once this callback returns.
        c->conn_ = nullptr;
        if (c->state_.load() == WTClient::State::kClosed)
        {
            return; // teardown already running (engine destroy path)
        }
        const WTClient::State state = c->state_.load();
        // A reason stashed by CloseGracefully (e.g. an HTTP-rejected
        // extended CONNECT) wins over the generic close reasons.
        const std::string reason = !c->pending_close_reason_.empty()
                                       ? c->pending_close_reason_
                                       : (state == WTClient::State::kClosing ? "closed locally" : "connection closed");
        c->ScheduleTeardown(reason);
    }

    static void OnHskDone(lsquic_conn_t* conn, enum lsquic_hsk_status status)
    {
        auto* c = FromConn(conn);
        if (status == LSQ_HSK_OK || status == LSQ_HSK_RESUMED_OK)
        {
            Log("handshake complete (hsk_status=%d); ALPN h3 implied by LSENG_HTTP engine", static_cast<int>(status));
            // Pin the minimum datagram-write size: with the default 0,
            // lsquic may call on_dg_write with only the leftover space of
            // a nearly-full packet (pf_gen_datagram_frame passes
            // MIN(avail, max)), which is not a transport ceiling - large
            // queued datagrams (SRT data packets) must wait for the next
            // fresh packet instead of being dropped. WebSRT requires
            // >= 1200-byte WT datagram support, so kMaxDatagramSize
            // always fits the peer's advertised frame maximum.
            if (lsquic_conn_set_min_datagram_size(conn, kMaxDatagramSize) != 0)
            {
                Log("could not pin min datagram size %zu; large datagrams may drop", kMaxDatagramSize);
            }
            lsquic_conn_make_stream(conn);
        }
        else
        {
            c->ScheduleTeardown("TLS handshake failed (status " + std::to_string(static_cast<int>(status)) + ")");
        }
    }

    static lsquic_stream_ctx_t* OnNewStream(void* ctx, lsquic_stream_t* stream)
    {
        auto* c       = static_cast<WTClient*>(ctx);
        c->stream_id_ = lsquic_stream_id(stream);
        lsquic_stream_wantread(stream, 1);
        lsquic_stream_wantwrite(stream, 1);
        return reinterpret_cast<lsquic_stream_ctx_t*>(c);
    }

    static void OnRead(lsquic_stream_t* stream, lsquic_stream_ctx_t* /*h*/)
    {
        auto* c = reinterpret_cast<WTClient*>(lsquic_stream_get_ctx(stream));
        c->ReadSessionStream(stream);
    }

    static void OnWrite(lsquic_stream_t* stream, lsquic_stream_ctx_t* /*h*/)
    {
        auto* c = reinterpret_cast<WTClient*>(lsquic_stream_get_ctx(stream));
        if (!c->headers_sent_)
        {
            c->SendConnectHeaders(stream);
            lsquic_stream_wantwrite(stream, 0);
        }
    }

    static void OnClose(lsquic_stream_t* stream, lsquic_stream_ctx_t* /*h*/)
    {
        auto* c = reinterpret_cast<WTClient*>(lsquic_stream_get_ctx(stream));
        Log("session stream closed");
        if (c->state_.load() == WTClient::State::kClosed)
        {
            return; // teardown already running (engine destroy path): the
                    // listener (session) may already be gone - don't touch it
        }
        if (!c->stream_terminal_)
        {
            c->stream_terminal_ = true;
            c->listener_.OnStreamReadable();
        }
    }

    static void OnDatagram(lsquic_conn_t* conn, const void* buf, std::size_t size)
    {
        FromConn(conn)->OnDatagramFromEngine(buf, size);
    }

    static ssize_t OnDgWrite(lsquic_conn_t* conn, void* buf, std::size_t size)
    {
        return FromConn(conn)->OnDgWriteFromEngine(buf, size);
    }

    static int VerifyCertPinned(void* ctx, struct stack_st_X509* chain)
    {
        auto* c    = static_cast<WTClient*>(ctx);
        X509* leaf = sk_X509_value(chain, 0);
        if (leaf == nullptr)
        {
            return -1;
        }
        unsigned char* der     = nullptr;
        const int      der_len = i2d_X509(leaf, &der);
        if (der_len <= 0 || der == nullptr)
        {
            return -1;
        }
        std::uint8_t md[SHA256_DIGEST_LENGTH];
        SHA256(der, static_cast<std::size_t>(der_len), md);
        OPENSSL_free(der);
        const bool match = std::memcmp(md, c->expected_hash_, sizeof md) == 0;
        Log("cert verify: presented %s pinned %s -> %s",
            HexStr(md, sizeof md).c_str(),
            HexStr(c->expected_hash_, sizeof c->expected_hash_).c_str(),
            match ? "MATCH" : "MISMATCH");
        return match ? 0 : -1;
    }

    // The system trust store, loaded once per process from the first
    // standard CA bundle that exists (read-only afterwards, so sharing it
    // across engines and threads is safe).
    static X509_STORE* SystemCaStore()
    {
        static const char* kCaBundles[] = {
            "/etc/ssl/certs/ca-certificates.crt", // Debian/Ubuntu
            "/etc/pki/tls/certs/ca-bundle.crt",   // Fedora/RHEL
            "/etc/ssl/ca-bundle.pem",             // openSUSE
        };
        static X509_STORE* store = []
        {
            X509_STORE* s = X509_STORE_new();
            if (s == nullptr)
            {
                return static_cast<X509_STORE*>(nullptr);
            }
            for (const char* path : kCaBundles)
            {
                X509_LOOKUP* lookup = X509_STORE_add_lookup(s, X509_LOOKUP_file());
                if (lookup != nullptr && X509_LOOKUP_load_file(lookup, path, X509_FILETYPE_PEM) == 1)
                {
                    return s;
                }
            }
            X509_STORE_free(s);
            return static_cast<X509_STORE*>(nullptr);
        }();
        return store;
    }

    // PKI mode: verify the presented chain against the system CAs and
    // require the target host (SNI) to match a certificate SAN.
    static int VerifyCertPki(void* ctx, struct stack_st_X509* chain)
    {
        auto* c    = static_cast<WTClient*>(ctx);
        X509* leaf = sk_X509_value(chain, 0);
        if (leaf == nullptr)
        {
            return -1;
        }
        X509_STORE* store = SystemCaStore();
        if (store == nullptr)
        {
            Log("cert verify: no system CA bundle found");
            return -1;
        }

        // The untrusted intermediates are the chain minus the leaf; refs
        // are bumped so the stack can be safely freed afterwards.
        STACK_OF(X509)* untrusted = sk_X509_new_null();
        if (untrusted == nullptr)
        {
            return -1;
        }
        for (std::size_t i = 1; i < static_cast<std::size_t>(sk_X509_num(chain)); ++i)
        {
            X509* cert = sk_X509_value(chain, i);
            if (cert == nullptr || X509_up_ref(cert) < 1)
            {
                continue;
            }
            sk_X509_push(untrusted, cert);
        }

        X509_STORE_CTX* csc     = X509_STORE_CTX_new();
        bool            name_ok = false;
        if (csc != nullptr && X509_STORE_CTX_init(csc, store, leaf, untrusted) == 1)
        {
            if (X509_verify_cert(csc) == 1)
            {
                sockaddr_in probe;
                if (inet_pton(AF_INET, c->peer_host_.c_str(), &probe.sin_addr) == 1)
                {
                    name_ok = X509_check_ip_asc(leaf, c->peer_host_.c_str(), 0) == 1;
                }
                else
                {
                    name_ok = X509_check_host(leaf, c->peer_host_.c_str(), c->peer_host_.size(), 0, nullptr) == 1;
                }
                if (!name_ok)
                {
                    Log("cert verify: chain ok but host \"%s\" matches no SAN", c->peer_host_.c_str());
                }
            }
            else
            {
                Log("cert verify: chain rejected: %s", X509_verify_cert_error_string(X509_STORE_CTX_get_error(csc)));
            }
        }
        X509_STORE_CTX_free(csc);
        sk_X509_pop_free(untrusted, X509_free);
        return name_ok ? 0 : -1;
    }

    static int PacketsOut(void* ctx, const struct lsquic_out_spec* specs, unsigned count)
    {
        auto* c    = static_cast<WTClient*>(ctx);
        int   sent = 0;
        for (unsigned i = 0; i < count; ++i)
        {
            const ssize_t n = ::send(c->fd_, specs[i].iov->iov_base, specs[i].iov->iov_len, 0);
            if (n < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    break;
                }
                return sent > 0 ? sent : -1;
            }
            ++sent;
        }
        return sent;
    }
};

const struct lsquic_stream_if WtClientCallbacks::kStreamIf = {
    .on_new_conn                = WtClientCallbacks::OnNewConn,
    .on_goaway_received         = nullptr,
    .on_conn_closed             = WtClientCallbacks::OnConnClosed,
    .on_new_stream              = WtClientCallbacks::OnNewStream,
    .on_read                    = WtClientCallbacks::OnRead,
    .on_write                   = WtClientCallbacks::OnWrite,
    .on_close                   = WtClientCallbacks::OnClose,
    .on_dg_write                = WtClientCallbacks::OnDgWrite,
    .on_datagram                = WtClientCallbacks::OnDatagram,
    .on_hsk_done                = WtClientCallbacks::OnHskDone,
    .on_new_token               = nullptr,
    .on_sess_resume_info        = nullptr,
    .on_reset                   = nullptr,
    .on_conncloseframe_received = nullptr,
    .on_hset_in                 = nullptr,
};

void WTClient::OnDatagramFromEngine(const void* buf, std::size_t size)
{
    const auto* p   = static_cast<const std::uint8_t*>(buf);
    const auto  qid = VarintGet(p, size);
    if (!qid || qid->second >= size)
    {
        return; // malformed context prefix; nothing to deliver
    }
    const std::uint8_t* payload     = p + qid->second;
    const std::size_t   payload_len = size - qid->second;
    datagrams_received_.fetch_add(1);
    listener_.OnDatagram(payload, payload_len);
}

ssize_t WTClient::OnDgWriteFromEngine(void* buf, std::size_t size)
{
    if (datagram_write_size_ == 0)
    {
        datagram_write_size_ = size;
        Log("datagram write size: %zu", size);
    }
    while (!send_queue_.empty())
    {
        std::vector<std::uint8_t>& front = send_queue_.front();
        if (front.size() > size)
        {
            send_queue_.pop_front();
            datagrams_dropped_.fetch_add(1);
            Log("dropping %zu-byte datagram over the %zu-byte transport ceiling", front.size(), size);
            continue;
        }
        const std::size_t n = front.size();
        std::memcpy(buf, front.data(), n);
        send_queue_.pop_front();
        datagrams_written_.fetch_add(1);
        lsquic_conn_want_datagram_write(conn_, send_queue_.empty() ? 0 : 1);
        return static_cast<ssize_t>(n);
    }
    lsquic_conn_want_datagram_write(conn_, 0);
    return 0;
}

WTClient::WTClient(EventLoop& loop, WTClientListener& listener)
    : loop_(loop)
    , listener_(listener)
{
}

WTClient::~WTClient()
{
    if (engine_ == nullptr)
    {
        return; // never connected, or already torn down (OnSessionClosed path)
    }
    assert(!loop_.OnLoopThread()); // never destroy from inside a callback
    if (loop_.IsRunning())
    {
        auto done = teardown_done_.get_future();
        loop_.Post(
            [this]
            {
                Teardown({}, false);
                teardown_done_.set_value();
            });
        done.wait();
    }
    else
    {
        Teardown({}, false);
    }
}

bool WTClient::Connect(const Target& target)
{
    if (state_.load() != State::kIdle)
    {
        return false;
    }
    // The dial address is the pre-resolved IP for a DNS-name host; `host`
    // stays the name as typed (SNI, :authority, PKI SAN check).
    const std::string& dial = target.resolved_ip.empty() ? target.host : target.resolved_ip;
    sockaddr_in        peer{};
    peer.sin_family = AF_INET;
    peer.sin_port   = htons(target.port);
    // Empty cert_hash_hex selects PKI validation; otherwise it must parse
    // as the pinned SHA-256 of the certificate DER.
    pin_cert_ = !target.cert_hash_hex.empty();
    if (target.port == 0 || target.path.empty() || inet_pton(AF_INET, dial.c_str(), &peer.sin_addr) != 1 ||
        (pin_cert_ && !ParseHashHex(target.cert_hash_hex, expected_hash_)))
    {
        return false;
    }
    State expected = State::kIdle;
    if (!state_.compare_exchange_strong(expected, State::kConnecting))
    {
        return false;
    }
    path_      = target.path;
    peer_host_ = target.host;
    authority_ = target.host + ":" + std::to_string(target.port);
    loop_.Post([this, target] { ConnectOnLoop(target); });
    return true;
}

void WTClient::ConnectOnLoop(const Target& target)
{
    static std::once_flag global_init_once;
    std::call_once(global_init_once,
                   []
                   {
                       if (0 != lsquic_global_init(LSQUIC_GLOBAL_CLIENT))
                       {
                           Log("lsquic_global_init failed");
                       }
                   });

    peer_sa_            = sockaddr_in{};
    peer_sa_.sin_family = AF_INET;
    peer_sa_.sin_port   = htons(target.port);
    // Same dial/name split as Connect: resolved IP into the socket, the
    // typed name into SNI (lsquic_engine_connect below).
    const std::string& dial = target.resolved_ip.empty() ? target.host : target.resolved_ip;
    if (inet_pton(AF_INET, dial.c_str(), &peer_sa_.sin_addr) != 1)
    {
        ScheduleTeardown("bad target dial address " + dial);
        return;
    }

    fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0)
    {
        ScheduleTeardown(std::string("socket failed: ") + std::strerror(errno));
        return;
    }
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&peer_sa_), sizeof peer_sa_) != 0)
    {
        ScheduleTeardown(std::string("UDP connect failed: ") + std::strerror(errno));
        return;
    }
    socklen_t len = sizeof local_sa_;
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&local_sa_), &len) != 0)
    {
        ScheduleTeardown(std::string("getsockname failed: ") + std::strerror(errno));
        return;
    }
    loop_.AddFd(fd_, EPOLLIN, [this](int, std::uint32_t) { HandleUdpReadable(); });
    fd_registered_ = true;

    struct lsquic_engine_settings settings;
    lsquic_engine_init_settings(&settings, LSENG_HTTP);
    settings.es_versions          = 1U << LSQVER_I001; // QUIC v1 only: the gateway speaks v1 + h3
    settings.es_datagrams         = 1;                 // advertise MAX_DATAGRAM_FRAME_SIZE transport param
    settings.es_enable_bw_sampler = 1;                 // we call lsquic_conn_get_info for the RTT
    char err_buf[256];
    if (0 != lsquic_engine_check_settings(&settings, LSENG_HTTP, err_buf, sizeof err_buf))
    {
        ScheduleTeardown(std::string("engine settings rejected: ") + err_buf);
        return;
    }

    struct lsquic_engine_api api{};
    api.ea_settings        = &settings;
    api.ea_packets_out     = WtClientCallbacks::PacketsOut;
    api.ea_packets_out_ctx = this;
    api.ea_stream_if       = &WtClientCallbacks::kStreamIf;
    api.ea_stream_if_ctx   = this;
    api.ea_verify_cert     = pin_cert_ ? WtClientCallbacks::VerifyCertPinned : WtClientCallbacks::VerifyCertPki;
    api.ea_verify_ctx      = this;
    engine_                = lsquic_engine_new(LSENG_HTTP, &api);
    if (engine_ == nullptr)
    {
        ScheduleTeardown("lsquic_engine_new failed");
        return;
    }
    loop_.SetEngineHost(this);

    conn_ = lsquic_engine_connect(engine_,
                                  N_LSQVER,
                                  reinterpret_cast<sockaddr*>(&local_sa_),
                                  reinterpret_cast<sockaddr*>(&peer_sa_),
                                  nullptr,
                                  nullptr,
                                  target.host.c_str(),
                                  0,
                                  nullptr,
                                  0,
                                  nullptr,
                                  0);
    if (conn_ == nullptr)
    {
        ScheduleTeardown("lsquic_engine_connect failed");
        return;
    }
    Log("connecting to %s (local %s:%u), path %s",
        authority_.c_str(),
        inet_ntoa(local_sa_.sin_addr),
        ntohs(local_sa_.sin_port),
        path_.c_str());
}

bool WTClient::SendDatagram(const void* data, std::size_t size)
{
    if (size == 0 || size > kMaxDatagramSize || state_.load() != State::kConnected)
    {
        return false;
    }
    std::vector<std::uint8_t> payload(static_cast<const std::uint8_t*>(data),
                                      static_cast<const std::uint8_t*>(data) + size);
    if (loop_.OnLoopThread())
    {
        EnqueueDatagram(std::move(payload));
    }
    else
    {
        loop_.Post([this, payload = std::move(payload)]() mutable { EnqueueDatagram(std::move(payload)); });
    }
    return true;
}

void WTClient::EnqueueDatagram(std::vector<std::uint8_t> payload)
{
    if (state_.load() != State::kConnected || conn_ == nullptr)
    {
        return; // the session closed under us; the datagram is moot
    }
    if (send_queue_.size() >= kMaxQueuedDatagrams)
    {
        datagrams_dropped_.fetch_add(1);
        return;
    }
    std::vector<std::uint8_t> framed;
    framed.reserve(payload.size() + 8);
    std::uint8_t      varint[8];
    const std::size_t varint_len = VarintPut(varint, stream_id_ >> 2);
    framed.insert(framed.end(), varint, varint + varint_len);
    framed.insert(framed.end(), payload.begin(), payload.end());
    send_queue_.push_back(std::move(framed));
    lsquic_conn_want_datagram_write(conn_, 1);
}

void WTClient::Close()
{
    const State state = state_.load();
    if (state != State::kConnecting && state != State::kConnected)
    {
        return;
    }
    if (loop_.OnLoopThread())
    {
        CloseOnLoop();
    }
    else
    {
        loop_.Post([this] { CloseOnLoop(); });
    }
}

void WTClient::CloseOnLoop()
{
    const State state = state_.load();
    if (state == State::kClosing || state == State::kClosed)
    {
        return;
    }
    if (conn_ != nullptr)
    {
        state_.store(State::kClosing);
        lsquic_conn_close(conn_);
    }
    else if (engine_ != nullptr)
    {
        ScheduleTeardown("closed before the connection came up");
    }
}

void WTClient::CloseGracefully(const std::string& reason)
{
    const State state = state_.load();
    if (state == State::kClosed || state == State::kClosing)
    {
        return; // a close is already in flight; the first reason wins
    }
    if (conn_ != nullptr)
    {
        pending_close_reason_ = reason;
        state_.store(State::kClosing);
        lsquic_conn_close(conn_);
    }
    else
    {
        ScheduleTeardown(reason);
    }
}

void WTClient::ScheduleTeardown(const std::string& reason)
{
    // Deferred: never destroy the engine from inside an engine callback.
    loop_.Post([this, reason] { Teardown(reason, true); });
}

void WTClient::Teardown(const std::string& reason, bool notify)
{
    if (state_.load() == State::kClosed)
    {
        return;
    }
    state_.store(State::kClosed);
    loop_.SetEngineHost(nullptr);
    if (fd_registered_)
    {
        loop_.DelFd(fd_);
        fd_registered_ = false;
    }
    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
    if (engine_ != nullptr)
    {
        // Fires remaining on_close / on_conn_closed callbacks; they no-op on
        // the kClosed state.
        lsquic_engine_destroy(engine_);
        engine_ = nullptr;
    }
    conn_      = nullptr;
    stream_id_ = UINT64_MAX;
    send_queue_.clear();
    if (notify)
    {
        Log("session closed: %s", reason.c_str());
        listener_.OnSessionClosed(reason);
    }
}

void WTClient::HandleUdpReadable()
{
    if (engine_ == nullptr)
    {
        return;
    }
    std::uint8_t buf[kRecvBufSize];
    for (;;)
    {
        const ssize_t n = ::recv(fd_, buf, sizeof buf, MSG_DONTWAIT);
        if (n > 0)
        {
            udp_packets_received_.fetch_add(1);
            lsquic_engine_packet_in(engine_,
                                    buf,
                                    static_cast<std::size_t>(n),
                                    reinterpret_cast<sockaddr*>(&local_sa_),
                                    reinterpret_cast<sockaddr*>(&peer_sa_),
                                    nullptr,
                                    0);
            continue;
        }
        return; // EAGAIN, or an ICMP error such as ECONNREFUSED: nothing to feed
    }
}

void WTClient::SendConnectHeaders(lsquic_stream_t* stream)
{
    // wtransport's exact extended-CONNECT set (wtransport-proto session.rs):
    // :method CONNECT, :scheme https, :protocol webtransport, :authority,
    // :path.
    const struct
    {
        const char* name;
        const char* val;
    } pairs[] = {
        {.name = ":method", .val = "CONNECT"},
        {.name = ":scheme", .val = "https"},
        {.name = ":protocol", .val = "webtransport"},
        {.name = ":authority", .val = authority_.c_str()},
        {.name = ":path", .val = path_.c_str()},
        {.name = "user-agent", .val = kUserAgent},
    };
    lsxpack_header headers[std::size(pairs)];
    char           storage[1024];
    std::size_t    off = 0;
    for (std::size_t i = 0; i < std::size(pairs); ++i)
    {
        const std::size_t name_len = std::strlen(pairs[i].name);
        const std::size_t val_len  = std::strlen(pairs[i].val);
        std::memcpy(storage + off, pairs[i].name, name_len);
        std::memcpy(storage + off + name_len, pairs[i].val, val_len);
        lsxpack_header_set_offset2(&headers[i], storage + off, 0, name_len, name_len, val_len);
        off += name_len + val_len;
    }
    lsquic_http_headers_t hdrs{};
    hdrs.count   = std::size(pairs);
    hdrs.headers = headers;
    if (0 == lsquic_stream_send_headers(stream, &hdrs, 0))
    {
        // With eos=0 the header block only lands in the stream buffer; it
        // must be flushed explicitly or it never reaches the wire.
        lsquic_stream_flush(stream);
        headers_sent_ = true;
        Log("extended CONNECT sent on stream %" PRIu64 ": %s (host %s)", stream_id_, path_.c_str(), authority_.c_str());
    }
    else
    {
        ScheduleTeardown(std::string("lsquic_stream_send_headers failed: ") + std::strerror(errno));
    }
}

void WTClient::ReadSessionStream(lsquic_stream_t* stream)
{
    std::uint8_t buf[kRecvBufSize];
    for (;;)
    {
        const ssize_t n = lsquic_stream_read(stream, buf, sizeof buf);
        if (n > 0)
        {
            if (!headers_done_)
            {
                const std::size_t consumed = response_parser_.Feed(buf, static_cast<std::size_t>(n));
                if (response_parser_.overflow())
                {
                    // A gateway that never terminates its header block
                    // must not grow the client's buffer without limit.
                    CloseGracefully(std::string("response header block exceeded ") +
                                    std::to_string(ConnectResponseParser::kMaxHeaderBytes) +
                                    " bytes without a terminator");
                    break;
                }
                if (consumed == 0)
                {
                    continue; // header block still incomplete
                }
                headers_done_    = true;
                const int status = response_parser_.status();
                http_status_.store(status);
                if (status >= 0)
                {
                    Log("session response: \"%s\" -> %d", response_parser_.status_line().c_str(), status);
                }
                capsule_parser_.Feed(buf + consumed, static_cast<std::size_t>(n) - consumed);
                NotifyCapsuleClose();
                if (status == 200)
                {
                    state_.store(State::kConnected);
                    listener_.OnSessionReady();
                }
                else if (status >= 0)
                {
                    // The connection is live: close it gracefully so
                    // the gateway sees CONNECTION_CLOSE instead of
                    // holding a zombie until its idle timeout.
                    CloseGracefully("HTTP status " + std::to_string(status));
                }
                else
                {
                    CloseGracefully("unparseable response status line: \"" + response_parser_.status_line() + "\"");
                }
            }
            else
            {
                capsule_parser_.Feed(buf, static_cast<std::size_t>(n));
                NotifyCapsuleClose();
            }
            if (state_.load() == State::kConnected && capsule_parser_.overflow())
            {
                // The parser stays bounded (it drops bytes once
                // overflowed), but the framing is untrustworthy: tear
                // the session down instead of lingering connected but
                // dead. The kConnected guard keeps an earlier, more
                // precise close reason (e.g. an HTTP status)
                // authoritative.
                CloseGracefully(std::string("session stream capsule framing demanded more than ") +
                                std::to_string(WtCapsuleParser::kMaxPendingBytes) + " pending bytes");
                break;
            }
            continue;
        }
        if (n == 0)
        {
            if (!stream_terminal_)
            {
                // Stop the read interest: a FIN'd stream stays readable
                // forever and would spin on_read otherwise.
                stream_terminal_ = true;
                lsquic_stream_wantread(stream, 0);
                Log("session stream FIN");
                listener_.OnStreamReadable();
            }
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            break;
        }
        Log("session stream read error: %s", std::strerror(errno));
        if (!stream_terminal_)
        {
            stream_terminal_ = true;
            lsquic_stream_wantread(stream, 0);
            listener_.OnStreamReadable();
        }
        break;
    }
}

// Terminal stream notification for a consumed CLOSE capsule: the parser's
// closed flag is sticky, stream_terminal_ makes the notify once.
void WTClient::NotifyCapsuleClose()
{
    if (capsule_parser_.closed() && !stream_terminal_)
    {
        stream_terminal_ = true;
        Log("WT CLOSE capsule received");
        listener_.OnStreamReadable();
    }
}

bool WTClient::NextTick(std::chrono::microseconds& diff)
{
    if (engine_ == nullptr)
    {
        return false;
    }
    int diff_us = 0;
    if (lsquic_engine_earliest_adv_tick(engine_, &diff_us) != 0)
    {
        diff = std::chrono::microseconds{diff_us};
        return true;
    }
    return false;
}

void WTClient::Pump()
{
    if (engine_ == nullptr)
    {
        return;
    }
    int diff_us = 0;
    if (lsquic_engine_earliest_adv_tick(engine_, &diff_us) != 0 && diff_us <= 0)
    {
        lsquic_engine_process_conns(engine_);
    }
    if (lsquic_engine_has_unsent_packets(engine_) != 0)
    {
        lsquic_engine_send_unsent_packets(engine_);
    }
    if (conn_ != nullptr)
    {
        struct lsquic_conn_info info;
        if (0 == lsquic_conn_get_info(conn_, &info))
        {
            rtt_us_.store(info.lci_rtt);
        }
    }
}

int WTClient::HttpStatus() const
{
    return http_status_.load();
}

std::chrono::microseconds WTClient::SmoothedRtt() const
{
    return std::chrono::microseconds{rtt_us_.load()};
}

int WTClient::DatagramsReceived() const
{
    return datagrams_received_.load();
}

int WTClient::DatagramsWritten() const
{
    return datagrams_written_.load();
}

int WTClient::DatagramsDropped() const
{
    return datagrams_dropped_.load();
}

int WTClient::UdpPacketsReceived() const
{
    return udp_packets_received_.load();
}

} // namespace websrt
