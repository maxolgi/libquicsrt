// wt_echo: P1 risk-retirement spike (websrt-cpp.md, Risks #1). A hand-driven
// lsquic HTTP/3 client opens a WebTransport session to the live Rust gateway
// (extended CONNECT with :protocol webtransport, cert-hash pinning), receives
// the gateway's SRT Conclusion handshake as WT datagrams, and sends garbage
// WT datagrams back. This validates the primitive the whole subscriber and
// publisher stack rests on.
//
// The oracle is the local supervisord `websrt` service (UDP 127.0.0.1:4433)
// plus its runtime cert-hash.js. When either is absent the test exits with
// code 77 (registered as ctest's SKIP_RETURN_CODE) so `./build.sh test` stays
// green on machines without the Rust world. With the oracle up, every check
// below must genuinely pass.

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

#include <lsxpack_header.h>
#include <lsquic.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{

constexpr int kSkipExit = 77;

// WT capsule protocol (RFC 9599). Only CLOSE terminates the session;
// PADDING and unknown types are skippable via the length prefix.
constexpr uint64_t kCapsuleCloseWtSession = 0x2843;

// Loopback oracle endpoints. The gateway host:port and cert-hash.js path can
// be overridden for remote-gateway experiments.
constexpr char kDefaultGateway[] = "127.0.0.1:4433";

constexpr std::chrono::milliseconds kTickCapMs{2};         // match the plan's ticker cadence
constexpr std::chrono::seconds      kHandshakeDeadline{5}; // no UDP life by then -> SKIP
constexpr std::chrono::seconds      kObserveWindow{3};     // datagram round-trip observation
constexpr std::chrono::milliseconds kSendPeriodMs{250};
constexpr std::chrono::seconds      kTotalDeadline{15}; // hard stop for the whole spike
constexpr size_t                    kRecvBufSize = 16384;

enum class Phase : std::uint8_t
{
    kHandshaking,
    kAwaitResponse,
    kObserving,
    kClosing,
    kDone,
};

struct App
{
    // oracle config, resolved once in main
    uint8_t expected_hash[SHA256_DIGEST_LENGTH] = {};

    // transport plumbing
    lsquic_engine_t* engine = nullptr;
    lsquic_conn_t*   conn   = nullptr;
    int              fd     = -1;
    sockaddr_in      local_sa{};
    sockaddr_in      peer_sa{};
    std::string      authority;

    // state machine timestamps
    std::chrono::steady_clock::time_point observe_start;
    std::chrono::steady_clock::time_point last_send;

    // session stream (extended CONNECT)
    uint64_t             stream_id   = UINT64_MAX;
    int                  http_status = -1;
    std::vector<uint8_t> session_buf; // accumulates headers remainder + capsules

    // WT datagrams received (payload after the quarter-stream-id prefix)
    int                  dg_in     = 0;
    size_t               dg_in_min = SIZE_MAX;
    size_t               dg_in_max = 0;
    int                  hist_lt64 = 0, hist_lt256 = 0, hist_lt1200 = 0, hist_lt1400 = 0, hist_ge1400 = 0;
    std::vector<uint8_t> first_dgram;
    uint64_t             first_dg_qid = UINT64_MAX;

    // WT datagrams sent
    std::vector<uint8_t> pending_dg;
    int                  dg_out_written = 0;

    // handshake evidence
    int cert_calls = 0;

    std::string fail_reason;

    // flags (grouped to keep the struct tight)
    Phase phase                        = Phase::kHandshaking;
    bool  observe_started              = false;
    bool  close_sent                   = false;
    bool  headers_sent                 = false;
    bool  headers_done                 = false;
    bool  close_capsule                = false;
    bool  stream_fin                   = false;
    bool  conn_closed                  = false;
    bool  first_dgram_is_srt_handshake = false;
    // Session end state snapshotted when the observation window completes;
    // our own clean teardown afterwards must not count against "stayed open".
    bool open_at_window_end = false;
    bool hsk_ok             = false;
    bool udp_life_seen      = false;
    bool cert_match         = false;
    bool skip               = false;

    void Fail(const std::string& why)
    {
        if (fail_reason.empty())
        {
            fail_reason = why;
        }
    }
};

App g_app;

void Log(const char* fmt, ...)
{
    std::fputs("wt_echo: ", stdout);
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fputc('\n', stdout);
}

std::string HexStr(const uint8_t* p, size_t n)
{
    static const char kHex[] = "0123456789abcdef";
    std::string       s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i)
    {
        s += kHex[p[i] >> 4];
        s += kHex[p[i] & 0xf];
    }
    return s;
}

// QUIC varint (RFC 9000 s.16): 2-bit length prefix, big-endian payload.
size_t VarintPut(uint8_t* dst, uint64_t v)
{
    if (v < 0x40)
    {
        dst[0] = static_cast<uint8_t>(v);
        return 1;
    }
    if (v < 0x4000)
    {
        dst[0] = static_cast<uint8_t>(0x40 | v >> 8);
        dst[1] = static_cast<uint8_t>(v);
        return 2;
    }
    if (v < 0x40000000)
    {
        dst[0] = static_cast<uint8_t>(0x80 | v >> 24);
        dst[1] = static_cast<uint8_t>(v >> 16);
        dst[2] = static_cast<uint8_t>(v >> 8);
        dst[3] = static_cast<uint8_t>(v);
        return 4;
    }
    dst[0] = static_cast<uint8_t>(0xc0 | v >> 56);
    dst[1] = static_cast<uint8_t>(v >> 48);
    dst[2] = static_cast<uint8_t>(v >> 40);
    dst[3] = static_cast<uint8_t>(v >> 32);
    dst[4] = static_cast<uint8_t>(v >> 24);
    dst[5] = static_cast<uint8_t>(v >> 16);
    dst[6] = static_cast<uint8_t>(v >> 8);
    dst[7] = static_cast<uint8_t>(v);
    return 8;
}

std::optional<std::pair<uint64_t, size_t>> VarintGet(const uint8_t* p, size_t n)
{
    if (n < 1)
    {
        return std::nullopt;
    }
    const uint8_t first = p[0];
    const size_t  len   = 1U << (first >> 6);
    if (n < len)
    {
        return std::nullopt;
    }
    uint64_t v = first & 0x3f;
    for (size_t i = 1; i < len; ++i)
    {
        v = v << 8 | p[i];
    }
    return std::make_pair(v, len);
}

// cert-hash.js: `window.CERT_HASH = "<64 hex chars>";` (self-signed mode).
// Returns nullopt when the file is missing, CERT_HASH is null (mkcert mode),
// or the value does not parse as a SHA-256 hex digest.
std::optional<std::string> LoadCertHashHex()
{
    const char* path = std::getenv("WEBSRT_CERT_HASH_JS");
    if (path == nullptr || *path == '\0')
    {
        path = WEBSRT_CERT_HASH_JS_DEFAULT;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        return std::nullopt;
    }
    const std::string js((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    const size_t key = js.find("CERT_HASH");
    if (key == std::string::npos)
    {
        return std::nullopt;
    }
    const size_t quote = js.find('"', key);
    if (quote == std::string::npos)
    {
        return std::nullopt;
    }
    const size_t hex_len = size_t{2} * SHA256_DIGEST_LENGTH;
    if (js.size() < quote + 1 + hex_len)
    {
        return std::nullopt;
    }
    std::string        hex        = js.substr(quote + 1, hex_len);
    static const char* kHexDigits = "0123456789abcdefABCDEF";
    for (const char c : hex)
    {
        if (std::strchr(kHexDigits, c) == nullptr)
        {
            return std::nullopt;
        }
    }
    return hex;
}

// ---- lsquic callbacks (all invoked from the engine calls on our thread) ----

int VerifyCert(void* ctx, struct stack_st_X509* chain)
{
    auto& app = *static_cast<App*>(ctx);
    ++app.cert_calls;
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
    uint8_t md[SHA256_DIGEST_LENGTH];
    SHA256(der, static_cast<size_t>(der_len), md);
    OPENSSL_free(der);
    const bool match = std::memcmp(md, app.expected_hash, sizeof md) == 0;
    if (match)
    {
        app.cert_match = true;
    }
    Log("cert verify: presented %s pinned %s -> %s",
        HexStr(md, sizeof md).c_str(),
        HexStr(app.expected_hash, sizeof app.expected_hash).c_str(),
        match ? "MATCH" : "MISMATCH");
    return match ? 0 : -1;
}

lsquic_conn_ctx_t* OnNewConn(void* /*stream_if_ctx*/, lsquic_conn_t* conn)
{
    g_app.conn = conn;
    return nullptr;
}

void OnConnClosed(lsquic_conn_t* /*conn*/)
{
    g_app.conn_closed = true;
    Log("connection closed");
}

void OnHskDone(lsquic_conn_t* conn, enum lsquic_hsk_status status)
{
    if (status == LSQ_HSK_OK || status == LSQ_HSK_RESUMED_OK)
    {
        g_app.hsk_ok = true;
        Log("handshake complete (hsk_status=%d); ALPN h3 implied by LSENG_HTTP engine", (int)status);
        lsquic_conn_make_stream(conn);
    }
    else
    {
        g_app.Fail("TLS handshake failed (status " + std::to_string((int)status) + ")");
        g_app.phase = Phase::kDone;
    }
}

void SendConnectHeaders(lsquic_stream_t* stream)
{
    auto& app = g_app;
    // wtransport's exact extended-CONNECT set (wtransport-proto session.rs):
    // :method CONNECT, :scheme https, :protocol webtransport, :authority,
    // :path. A fresh ?publish= name is accepted first-wins; a ?stream= name
    // with no live source would be rejected 404 before the WT handshake.
    const std::string path = "/wt?publish=wt_echo_" + std::to_string(static_cast<long>(::getpid()));
    const struct
    {
        const char* name;
        const char* val;
    } pairs[] = {
        {.name = ":method", .val = "CONNECT"},
        {.name = ":scheme", .val = "https"},
        {.name = ":protocol", .val = "webtransport"},
        {.name = ":authority", .val = app.authority.c_str()},
        {.name = ":path", .val = path.c_str()},
        {.name = "user-agent", .val = "wt_echo_spike"},
    };
    lsxpack_header headers[std::size(pairs)];
    char           storage[1024];
    size_t         off = 0;
    for (size_t i = 0; i < std::size(pairs); ++i)
    {
        const size_t name_len = std::strlen(pairs[i].name);
        const size_t val_len  = std::strlen(pairs[i].val);
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
        app.headers_sent = true;
        Log("extended CONNECT sent on stream %" PRIu64 ": %s (host %s)",
            app.stream_id,
            path.c_str(),
            app.authority.c_str());
    }
    else
    {
        app.Fail("lsquic_stream_send_headers failed: " + std::string(std::strerror(errno)));
        lsquic_stream_close(stream);
    }
}

// Minimal capsule consumption: type + length varints, skip the payload.
// CLOSE (0x2843) is the only type that changes session state. This is
// deliberately not a full capsule state machine - spike scope is datagrams.
void ParseCapsules(const uint8_t* p, size_t n)
{
    auto& app = g_app;
    app.session_buf.insert(app.session_buf.end(), p, p + n);
    size_t off = 0;
    for (;;)
    {
        const auto type = VarintGet(app.session_buf.data() + off, app.session_buf.size() - off);
        if (!type)
        {
            break;
        }
        const auto length =
            VarintGet(app.session_buf.data() + off + type->second, app.session_buf.size() - off - type->second);
        if (!length)
        {
            break;
        }
        if (app.session_buf.size() - off - type->second - length->second < length->first)
        {
            break;
        }
        if (type->first == kCapsuleCloseWtSession)
        {
            app.close_capsule = true;
            Log("WT CLOSE capsule received (%u payload bytes)", (unsigned)length->first);
        }
        off += type->second + length->second + length->first;
    }
    app.session_buf.erase(app.session_buf.begin(), app.session_buf.begin() + static_cast<std::ptrdiff_t>(off));
}

// With the built-in (HTTP/1.x) header interface the response pseudo-headers
// arrive as a "HTTP/1.1 200 ..." status line followed by regular headers and
// a blank line; anything after that is the session stream's capsule stream.
// Returns the number of bytes consumed as the header block, or 0 if the
// header terminator has not arrived yet.
size_t ParseResponseHeaders(const std::vector<uint8_t>& in)
{
    static const char kTerm[] = "\r\n\r\n";
    const auto        it      = std::search(in.begin(), in.end(), kTerm, kTerm + 4);
    if (it == in.end())
    {
        return 0;
    }
    const size_t consumed = static_cast<size_t>(it - in.begin()) + 4;

    const auto   line_end = std::ranges::find(in, '\r');
    char         line[32] = {};
    const size_t line_len = std::min<size_t>(static_cast<size_t>(line_end - in.begin()), sizeof line - 1);
    std::memcpy(line, in.data(), line_len);
    int  status      = -1;
    char version[16] = {};
    if (2 == std::sscanf(line, "%15s %d", version, &status))
    {
        g_app.http_status = status;
        Log("session response: \"%s\" -> %d", line, status);
    }
    else
    {
        g_app.Fail("unparseable response status line: \"" + std::string(line) + "\"");
    }
    return consumed;
}

void OnRead(lsquic_stream_t* stream, lsquic_stream_ctx_t* /*h*/)
{
    auto&   app = g_app;
    uint8_t buf[kRecvBufSize];
    for (;;)
    {
        const ssize_t n = lsquic_stream_read(stream, buf, sizeof buf);
        if (n > 0)
        {
            if (!app.headers_done)
            {
                app.session_buf.insert(app.session_buf.end(), buf, buf + n);
                const size_t consumed = ParseResponseHeaders(app.session_buf);
                if (consumed > 0)
                {
                    app.headers_done = true;
                    ParseCapsules(app.session_buf.data() + consumed, app.session_buf.size() - consumed);
                    app.session_buf.clear();
                }
            }
            else
            {
                ParseCapsules(buf, static_cast<size_t>(n));
            }
            continue;
        }
        if (n == 0)
        {
            app.stream_fin = true;
            Log("session stream FIN");
            break;
        }
        if (errno == EAGAIN)
        {
            break;
        }
        app.stream_fin = true;
        app.Fail("stream read error: " + std::string(std::strerror(errno)));
        break;
    }
}

lsquic_stream_ctx_t* OnNewStream(void* /*stream_if_ctx*/, lsquic_stream_t* stream)
{
    g_app.stream_id = lsquic_stream_id(stream);
    lsquic_stream_wantread(stream, 1);
    lsquic_stream_wantwrite(stream, 1);
    return nullptr;
}

void OnWrite(lsquic_stream_t* stream, lsquic_stream_ctx_t* /*h*/)
{
    if (!g_app.headers_sent)
    {
        SendConnectHeaders(stream);
        lsquic_stream_wantwrite(stream, 0);
    }
}

void OnClose(lsquic_stream_t* /*stream*/, lsquic_stream_ctx_t* /*h*/)
{
    Log("session stream closed");
}

void OnDatagram(lsquic_conn_t* /*conn*/, const void* buf, size_t sz)
{
    auto&       app = g_app;
    const auto* p   = static_cast<const uint8_t*>(buf);
    const auto  qid = VarintGet(p, sz);
    if (!qid || qid->second >= sz)
    {
        return; // malformed context prefix; nothing to measure
    }
    const uint8_t* payload     = p + qid->second;
    const size_t   payload_len = sz - qid->second;
    ++app.dg_in;
    app.dg_in_min = std::min(app.dg_in_min, payload_len);
    app.dg_in_max = std::max(app.dg_in_max, payload_len);
    if (payload_len < 64)
    {
        ++app.hist_lt64;
    }
    else if (payload_len < 256)
    {
        ++app.hist_lt256;
    }
    else if (payload_len < 1200)
    {
        ++app.hist_lt1200;
    }
    else if (payload_len < 1400)
    {
        ++app.hist_lt1400;
    }
    else
    {
        ++app.hist_ge1400;
    }
    if (app.first_dgram.empty())
    {
        app.first_dgram.assign(payload, payload + payload_len);
        app.first_dg_qid = qid->first;
        if (!app.observe_started)
        {
            app.observe_started = true;
            app.observe_start   = std::chrono::steady_clock::now();
        }
    }
}

ssize_t OnDgWrite(lsquic_conn_t* conn, void* buf, size_t sz)
{
    auto& app = g_app;
    if (app.pending_dg.empty())
    {
        lsquic_conn_want_datagram_write(conn, 0);
        return 0;
    }
    const size_t n = std::min(sz, app.pending_dg.size());
    std::memcpy(buf, app.pending_dg.data(), n);
    app.pending_dg.clear();
    lsquic_conn_want_datagram_write(conn, 0);
    ++app.dg_out_written;
    return static_cast<ssize_t>(n);
}

int LoggerLogBuf(void* /*ctx*/, const char* buf, size_t len)
{
    std::fwrite(buf, 1, len, stderr);
    return static_cast<int>(len);
}

const struct lsquic_stream_if kStreamIf = {
    .on_new_conn                = OnNewConn,
    .on_goaway_received         = nullptr,
    .on_conn_closed             = OnConnClosed,
    .on_new_stream              = OnNewStream,
    .on_read                    = OnRead,
    .on_write                   = OnWrite,
    .on_close                   = OnClose,
    .on_dg_write                = OnDgWrite,
    .on_datagram                = OnDatagram,
    .on_hsk_done                = OnHskDone,
    .on_new_token               = nullptr,
    .on_sess_resume_info        = nullptr,
    .on_reset                   = nullptr,
    .on_conncloseframe_received = nullptr,
    .on_hset_in                 = nullptr,
};

int PacketsOut(void* /*packets_out_ctx*/, const struct lsquic_out_spec* specs, unsigned count)
{
    int sent = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        const ssize_t n = ::send(g_app.fd, specs[i].iov->iov_base, specs[i].iov->iov_len, 0);
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

// ---- socket + engine plumbing ----

bool SetupSocket(const std::string& gateway)
{
    auto&             app   = g_app;
    const size_t      colon = gateway.rfind(':');
    const std::string host  = colon == std::string::npos ? gateway : gateway.substr(0, colon);
    const int         port  = colon == std::string::npos ? 4433 : std::atoi(gateway.c_str() + colon + 1);
    app.fd                  = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (app.fd < 0)
    {
        return false;
    }
    std::memset(&app.peer_sa, 0, sizeof app.peer_sa);
    app.peer_sa.sin_family = AF_INET;
    app.peer_sa.sin_port   = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &app.peer_sa.sin_addr) != 1)
    {
        return false;
    }
    sockaddr_in bind_addr{};
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind_addr.sin_port        = 0;
    if (::bind(app.fd, reinterpret_cast<sockaddr*>(&bind_addr), sizeof bind_addr) != 0)
    {
        return false;
    }
    if (::connect(app.fd, reinterpret_cast<sockaddr*>(&app.peer_sa), sizeof app.peer_sa) != 0)
    {
        return false;
    }
    socklen_t len = sizeof app.local_sa;
    if (::getsockname(app.fd, reinterpret_cast<sockaddr*>(&app.local_sa), &len) != 0)
    {
        return false;
    }
    app.authority = gateway;
    return true;
}

// Drains the socket into the engine; on_datagram fires during packet_in.
void PumpUdp()
{
    auto&   app = g_app;
    uint8_t buf[kRecvBufSize];
    for (;;)
    {
        const ssize_t n = ::recv(app.fd, buf, sizeof buf, MSG_DONTWAIT);
        if (n > 0)
        {
            app.udp_life_seen = true;
            lsquic_engine_packet_in(app.engine,
                                    buf,
                                    static_cast<size_t>(n),
                                    reinterpret_cast<sockaddr*>(&app.local_sa),
                                    reinterpret_cast<sockaddr*>(&app.peer_sa),
                                    nullptr,
                                    0);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            return;
        }
        if (n < 0 && errno == ECONNREFUSED)
        {
            // ICMP port unreachable: nothing is listening on the gateway port.
            return;
        }
        return;
    }
}

void ArmGarbageDatagram()
{
    auto& app = g_app;
    if (app.stream_id == UINT64_MAX)
    {
        return;
    }
    app.pending_dg.clear();
    const uint64_t quarter_stream_id = app.stream_id >> 2;
    uint8_t        varint[8];
    const size_t   varint_len = VarintPut(varint, quarter_stream_id);
    app.pending_dg.insert(app.pending_dg.end(), varint, varint + varint_len);
    const char garbage[] = "wt-echo non-SRT garbage";
    app.pending_dg.insert(app.pending_dg.end(), garbage, garbage + sizeof garbage - 1);
    lsquic_conn_want_datagram_write(app.conn, 1);
}

void LogFirstDatagramAnatomy()
{
    auto& app = g_app;
    if (app.first_dgram.empty())
    {
        return;
    }
    const uint8_t* p = app.first_dgram.data();
    const size_t   n = app.first_dgram.size();
    Log("first datagram: %u bytes, wt quarter-stream-id %llu, hex prefix: %s",
        (unsigned)n,
        (unsigned long long)app.first_dg_qid,
        HexStr(p, std::min<size_t>(n, 24)).c_str());
    // SRT control packet (big-endian): 16-byte header, then HandshakeControlInfo
    // per srt-protocol's HandshakeControlInfo::serialize.
    if (n >= 40)
    {
        const bool     control     = (p[0] & 0x80) != 0;
        const unsigned packet_type = ((p[0] & 0x7f) << 8) | p[1];
        const unsigned version     = (p[16] << 24) | (p[17] << 16) | (p[18] << 8) | p[19];
        const unsigned type_flags  = (p[20] << 24) | (p[21] << 16) | (p[22] << 8) | p[23];
        const unsigned shake_type  = (p[36] << 24) | (p[37] << 16) | (p[38] << 8) | p[39];
        const char*    shake_name  = "?";
        if (shake_type == 0xffffffff)
        {
            shake_name = "CONCLUSION";
        }
        else if (shake_type == 0)
        {
            shake_name = "INDUCTION";
        }
        Log("first datagram anatomy: control=%u packet_type=%u (0=handshake) version=%u type_flags=0x%08x "
            "shake_type=0x%08x (%s)",
            (int)control,
            packet_type,
            version,
            type_flags,
            shake_type,
            shake_name);
        app.first_dgram_is_srt_handshake = control && packet_type == 0 && version == 5 && shake_type == 0xffffffff;
    }
}

int Evaluate()
{
    auto& app = g_app;
    Log("-- evidence ------------------------------------------------------");
    Log("handshake ok                  : %s", app.hsk_ok ? "yes" : "NO");
    Log("cert pin                      : %s (%d verify call(s))", app.cert_match ? "MATCH" : "NO", app.cert_calls);
    Log("extended CONNECT headers sent : %s (stream id %llu)",
        app.headers_sent ? "yes" : "NO",
        (unsigned long long)app.stream_id);
    Log("HTTP response status          : %d", app.http_status);
    Log("WT datagrams in               : %d (sizes min..max %u..%u; <64:%d <256:%d <1200:%d <1400:%d >=1400:%d)",
        app.dg_in,
        (unsigned)app.dg_in_min,
        (unsigned)app.dg_in_max,
        app.hist_lt64,
        app.hist_lt256,
        app.hist_lt1200,
        app.hist_lt1400,
        app.hist_ge1400);
    LogFirstDatagramAnatomy();
    Log("WT datagrams out written      : %d", app.dg_out_written);
    Log("session end state             : close_capsule=%d stream_fin=%d conn_closed=%d",
        (int)app.close_capsule,
        (int)app.stream_fin,
        (int)app.conn_closed);
    Log("------------------------------------------------------------------");

    bool ok      = true;
    auto require = [&ok](bool cond, const char* what)
    {
        Log("%-42s: %s", what, cond ? "PASS" : "FAIL");
        ok = ok && cond;
    };
    require(app.hsk_ok, "TLS+QUIC handshake completed");
    require(app.cert_match, "cert hash pinned (SHA-256 of DER)");
    require(app.headers_sent, "extended CONNECT headers sent");
    require(app.http_status == 200, "session accepted (HTTP 200)");
    require(app.dg_in >= 1, "received >= 1 WT datagram");
    require(app.first_dgram_is_srt_handshake, "first datagram is SRT HSv5 Conclusion");
    require(app.dg_out_written >= 1, "sent >= 1 WT datagram (datagram-write path)");
    require(app.open_at_window_end, "session stayed open (observation window)");
    if (!app.fail_reason.empty())
    {
        Log("FAIL: %s", app.fail_reason.c_str());
        ok = false;
    }
    if (ok)
    {
        Log("wt_echo: ALL ASSERTIONS PASSED");
    }
    return ok ? 0 : 1;
}

} // namespace

int main()
{
    auto& app   = g_app;
    using clock = std::chrono::steady_clock;

    const auto hash_hex = LoadCertHashHex();
    if (!hash_hex)
    {
        Log("SKIP: cert-hash.js unavailable or not in self-signed mode");
        return kSkipExit;
    }
    auto nibble = [](char c) -> int
    {
        if (c >= '0' && c <= '9')
        {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f')
        {
            return c - 'a' + 10;
        }
        return c - 'A' + 10;
    };
    for (size_t i = 0; i < hash_hex->size() / 2; ++i)
    {
        app.expected_hash[i] =
            static_cast<uint8_t>((nibble((*hash_hex)[2 * i]) << 4) | nibble((*hash_hex)[(2 * i) + 1]));
    }
    Log("pinned cert hash: %s", hash_hex->c_str());

    const char*       gateway_env = std::getenv("WEBSRT_WT_GATEWAY");
    const std::string gateway     = gateway_env != nullptr && *gateway_env != '\0' ? gateway_env : kDefaultGateway;
    if (!SetupSocket(gateway))
    {
        Log("SKIP: cannot set up UDP socket for %s (%s)", gateway.c_str(), std::strerror(errno));
        return kSkipExit;
    }
    Log("gateway %s, local %s:%u", gateway.c_str(), inet_ntoa(app.local_sa.sin_addr), ntohs(app.local_sa.sin_port));

    if (const char* level = std::getenv("WT_ECHO_LSQUIC_LOG"))
    {
        static const struct lsquic_logger_if kLogger = {.log_buf = LoggerLogBuf};
        lsquic_logger_init(&kLogger, nullptr, LLTS_NONE);
        lsquic_set_log_level(level);
    }

    if (0 != lsquic_global_init(LSQUIC_GLOBAL_CLIENT))
    {
        Log("FAIL: lsquic_global_init");
        return 1;
    }

    struct lsquic_engine_settings settings;
    lsquic_engine_init_settings(&settings, LSENG_HTTP);
    settings.es_versions  = 1U << LSQVER_I001; // QUIC v1 only: the oracle speaks v1 + h3
    settings.es_datagrams = 1;                 // advertise MAX_DATAGRAM_FRAME_SIZE transport param
    char err_buf[256];
    if (0 != lsquic_engine_check_settings(&settings, LSENG_HTTP, err_buf, sizeof err_buf))
    {
        Log("FAIL: engine settings: %s", err_buf);
        return 1;
    }

    struct lsquic_engine_api api{};
    api.ea_settings      = &settings;
    api.ea_packets_out   = PacketsOut;
    api.ea_stream_if     = &kStreamIf;
    api.ea_stream_if_ctx = &app;
    api.ea_verify_cert   = VerifyCert;
    api.ea_verify_ctx    = &app;
    app.engine           = lsquic_engine_new(LSENG_HTTP, &api);
    if (app.engine == nullptr)
    {
        Log("FAIL: lsquic_engine_new");
        return 1;
    }

    const auto t0 = clock::now();
    app.conn      = lsquic_engine_connect(app.engine,
                                     N_LSQVER,
                                     reinterpret_cast<sockaddr*>(&app.local_sa),
                                     reinterpret_cast<sockaddr*>(&app.peer_sa),
                                     nullptr,
                                     nullptr,
                                     "127.0.0.1",
                                     0,
                                     nullptr,
                                     0,
                                     nullptr,
                                     0);
    if (app.conn == nullptr)
    {
        Log("FAIL: lsquic_engine_connect");
        return 1;
    }

    while (app.phase != Phase::kDone)
    {
        const auto now     = clock::now();
        const auto elapsed = now - t0;
        if (elapsed >= kTotalDeadline)
        {
            app.Fail("hard deadline reached in phase " + std::to_string((int)app.phase));
            break;
        }

        // Phase transitions that depend on evidence gathered so far.
        if (app.phase == Phase::kHandshaking && !app.udp_life_seen && elapsed >= kHandshakeDeadline)
        {
            Log("SKIP: no UDP response from %s within %lld s",
                gateway.c_str(),
                (long long)std::chrono::duration_cast<std::chrono::seconds>(kHandshakeDeadline).count());
            app.skip = true;
            break;
        }
        if (app.phase == Phase::kHandshaking && app.headers_sent)
        {
            app.phase = Phase::kAwaitResponse;
        }
        if (app.phase == Phase::kAwaitResponse && app.dg_in >= 1)
        {
            app.phase = Phase::kObserving;
        }
        if (app.phase == Phase::kObserving && app.observe_started && now - app.observe_start >= kObserveWindow)
        {
            app.open_at_window_end = !app.close_capsule && !app.stream_fin && !app.conn_closed;
            app.phase              = Phase::kClosing;
            Log("observation window complete (session open: %s); closing cleanly",
                app.open_at_window_end ? "yes" : "no");
        }
        if (app.phase == Phase::kClosing && !app.close_sent)
        {
            app.close_sent = true;
            ArmGarbageDatagram(); // one last round-trip attempt during drain
            lsquic_conn_close(app.conn);
        }

        // Keep the datagram-write path exercised while observing.
        if (app.phase == Phase::kObserving && now - app.last_send >= kSendPeriodMs)
        {
            app.last_send = now;
            ArmGarbageDatagram();
        }

        lsquic_engine_process_conns(app.engine);
        lsquic_engine_send_unsent_packets(app.engine);

        if (app.conn_closed)
        {
            app.phase = Phase::kDone;
        }

        // Wait: at most kTickCapMs, or until the engine wants to tick.
        int       diff_us    = 0;
        const int wants_tick = lsquic_engine_earliest_adv_tick(app.engine, &diff_us);
        int       wait_ms    = kTickCapMs.count();
        if (wants_tick != 0 && diff_us >= 0)
        {
            wait_ms = std::min<int>(wait_ms, (diff_us + 999) / 1000);
        }
        pollfd pfd{.fd = app.fd, .events = POLLIN, .revents = 0};
        ::poll(&pfd, 1, wait_ms);
        PumpUdp();
    }

    if (app.skip)
    {
        lsquic_engine_destroy(app.engine);
        lsquic_global_cleanup();
        ::close(app.fd);
        return kSkipExit;
    }

    // Give the CONNECTION_CLOSE a moment to drain.
    for (int i = 0; i < 150 && !app.conn_closed; ++i)
    {
        pollfd pfd{.fd = app.fd, .events = POLLIN, .revents = 0};
        ::poll(&pfd, 1, 2);
        PumpUdp();
        lsquic_engine_process_conns(app.engine);
        lsquic_engine_send_unsent_packets(app.engine);
    }

    const int verdict = Evaluate();
    lsquic_engine_destroy(app.engine);
    lsquic_global_cleanup();
    ::close(app.fd);
    return verdict;
}
