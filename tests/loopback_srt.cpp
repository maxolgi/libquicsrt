// M0 loopback: two forked-srt sockets connected skip-induction over an
// in-memory wire pair (virtual transport seam, zero OS sockets). Gate
// criteria: handshake, byte-exact 20000 x 1128 stream, NAK-driven loss
// recovery of one swallowed packet, SRTO_INITIAL_RTT seeding, negotiated
// payload readback, fd-count proof that no OS socket was created.
//
// Plain checks (not assert) so the test stays meaningful in Release.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <dirent.h>
#include <unistd.h>

#include "srt.h"

namespace
{

constexpr int     kPayloadSize  = 1128; // 6 x 188-byte TS packets - the WebSRT constant
constexpr int     kNumMessages  = 20000;
constexpr int     kVictimIndex  = kNumMessages / 2; // the swallowed original lives mid-stream
constexpr int     kLatencyMs    = 120;              // gateway ingest default; TSBPD window for NAK recovery
constexpr int64_t kMaxBw        = 8000000;          // bytes/s wire cap (~64 Mbit/s): 20000 msgs flow in ~3 s
constexpr int     kSndBufPkts   = 128; // bounds the send-queue backlog so retransmits get a wire slot in time
constexpr int     kSndBufBytes  = kSndBufPkts * (1500 - 28); // SRTO_SNDBUF is in bytes (mss - UDP header)
constexpr int     kSeedRttMs    = 300;
constexpr int     kTimeoutMs    = 5000; // fail fast instead of hanging
constexpr char    kSwallowTag[] = "M0-SWALLOW-ME";

std::atomic<bool> g_ok{true};

void Fail(const std::string& what)
{
    g_ok = false;
    std::fprintf(stderr, "loopback_srt: FAIL: %s\n", what.c_str());
}

// Every check site lives in a bool-returning phase function; returning false
// unwinds to main, where the RAII session tears the sockets down.
#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            Fail(std::string(#cond " [") + srt_getlasterror_str() + "]");                                              \
            return false;                                                                                              \
        }                                                                                                              \
    } while (false)

// In-memory "wire": routes what one socket sends into the peer's transport,
// reporting a fixed fake source address. The addresses never reach the
// network; they only label the endpoints for SRT's dispatching.
struct Wire
{
    SRTVirtualTransport* peer        = nullptr;
    sockaddr_storage     source_addr = {};
    socklen_t            source_len  = 0;
    // The first datagram carrying this byte sequence is swallowed (one-shot
    // loss); retransmissions carry the same payload and pass through.
    const char* swallow_tag = nullptr;
    int         swallowed   = 0;
};

int WireSend(void* opaq, const char* data, int len, const struct sockaddr* /*dst*/, int /*dstlen*/)
{
    Wire* w = static_cast<Wire*>(opaq);
    if (w->swallow_tag != nullptr && w->swallowed == 0 &&
        std::search(data, data + len, w->swallow_tag, w->swallow_tag + std::strlen(w->swallow_tag)) != data + len)
    {
        w->swallowed = 1;
        return len;
    }

    if (srt_virtual_transport_inject(
            w->peer, data, len, reinterpret_cast<const sockaddr*>(&w->source_addr), static_cast<int>(w->source_len)) ==
        -1)
    {
        return -1;
    }

    return len;
}

sockaddr_in FillV4(const char* ip, uint16_t port)
{
    sockaddr_in sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(port);
    sa.sin_addr.s_addr = inet_addr(ip);
    return sa;
}

void FillWireSource(Wire& w, const sockaddr_in& sa)
{
    std::memset(&w.source_addr, 0, sizeof w.source_addr);
    std::memcpy(&w.source_addr, &sa, sizeof sa);
    w.source_len = sizeof sa;
}

// Deterministic content: a xorshift32 stream seeded from the message index,
// regenerated identically on both sides. The victim message also carries the
// swallow tag so the wire can drop exactly its first transmission.
void FillMessage(int index, uint32_t* words)
{
    uint32_t state = 0x9E3779B9U ^ (static_cast<uint32_t>(index) * 2654435761U);
    for (int i = 0; i < kPayloadSize / 4; ++i)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        words[i] = state;
    }
    if (index == kVictimIndex)
    {
        std::memcpy(words, kSwallowTag, sizeof kSwallowTag - 1);
    }
}

// The sockets must be closed before the transports are released: srt_close
// sends the shutdown through the wire callbacks, which touch the wires.
struct Session
{
    SRTSOCKET            listener    = SRT_INVALID_SOCK;
    SRTSOCKET            caller      = SRT_INVALID_SOCK;
    SRTSOCKET            accepted    = SRT_INVALID_SOCK;
    SRTVirtualTransport* listener_vt = nullptr;
    SRTVirtualTransport* caller_vt   = nullptr;
    Wire                 caller_wire;
    Wire                 listener_wire;

    ~Session()
    {
        if (accepted != SRT_INVALID_SOCK)
        {
            srt_close(accepted);
        }
        if (caller != SRT_INVALID_SOCK)
        {
            srt_close(caller);
        }
        if (listener != SRT_INVALID_SOCK)
        {
            srt_close(listener);
        }
        if (caller_vt != nullptr)
        {
            srt_virtual_transport_release(caller_vt);
        }
        if (listener_vt != nullptr)
        {
            srt_virtual_transport_release(listener_vt);
        }
    }
};

// Counts the open file descriptors of the current process that are sockets.
int CountSocketFds()
{
    DIR* dir = opendir("/proc/self/fd");
    if (dir == nullptr)
    {
        return -1;
    }

    int            count = 0;
    struct dirent* entry;
    char           link[64];
    while ((entry = readdir(dir)) != nullptr)
    {
        if (entry->d_name[0] == '.')
        {
            continue;
        }
        const std::string path  = std::string("/proc/self/fd/") + entry->d_name;
        const ssize_t     nread = readlink(path.c_str(), link, sizeof link - 1);
        if (nread <= 0)
        {
            continue;
        }
        link[nread] = 0;
        if (strncmp(link, "socket:", 7) == 0)
        {
            ++count;
        }
    }
    closedir(dir);
    return count;
}

bool SetOptInt(SRTSOCKET u, SRT_SOCKOPT opt, int value)
{
    return srt_setsockopt(u, 0, opt, &value, sizeof value) != SRT_ERROR;
}

bool SetOptI64(SRTSOCKET u, SRT_SOCKOPT opt, int64_t value)
{
    return srt_setsockopt(u, 0, opt, &value, sizeof value) != SRT_ERROR;
}

// (a) Handshake: skip-induction caller (Conclusion-first) against an
// attached listener explicitly bound to a label address the caller then
// connects to.
bool ConnectPair(Session& s, const sockaddr_in& listener_label, const sockaddr_in& caller_addr)
{
    const int yes = 1;

    s.listener = srt_create_socket();
    CHECK(s.listener != SRT_INVALID_SOCK);
    CHECK(SetOptInt(s.listener, SRTO_SKIP_INDUCTION, yes));
    CHECK(SetOptInt(s.listener, SRTO_PAYLOADSIZE, kPayloadSize));
    CHECK(SetOptInt(s.listener, SRTO_LATENCY, kLatencyMs));
    s.listener_vt = srt_attach_virtual_transport(s.listener, WireSend, &s.listener_wire);
    CHECK(s.listener_vt != nullptr);
    CHECK(srt_bind(s.listener, (const sockaddr*)&listener_label, (int)sizeof listener_label) != SRT_ERROR);
    CHECK(srt_listen(s.listener, 5) != SRT_ERROR);

    s.caller = srt_create_socket();
    CHECK(s.caller != SRT_INVALID_SOCK);
    CHECK(SetOptInt(s.caller, SRTO_SKIP_INDUCTION, yes));
    CHECK(SetOptInt(s.caller, SRTO_PAYLOADSIZE, kPayloadSize));
    CHECK(SetOptInt(s.caller, SRTO_LATENCY, kLatencyMs));
    CHECK(SetOptInt(s.caller, SRTO_INITIAL_RTT, kSeedRttMs));
    CHECK(SetOptInt(s.caller, SRTO_SNDTIMEO, kTimeoutMs));
    CHECK(SetOptI64(s.caller, SRTO_MAXBW, kMaxBw));
    CHECK(SetOptInt(s.caller, SRTO_SNDBUF, kSndBufBytes));
    s.caller_vt = srt_attach_virtual_transport(s.caller, WireSend, &s.caller_wire);
    CHECK(s.caller_vt != nullptr);

    s.caller_wire.peer = s.listener_vt;
    FillWireSource(s.caller_wire, caller_addr);
    s.listener_wire.peer = s.caller_vt;
    FillWireSource(s.listener_wire, listener_label);
    s.caller_wire.swallow_tag = kSwallowTag;

    CHECK(srt_connect(s.caller,
                      reinterpret_cast<const sockaddr*>(&listener_label),
                      static_cast<int>(sizeof listener_label)) != SRT_ERROR);

    sockaddr_in accept_addr;
    int         accept_len = sizeof accept_addr;
    s.accepted             = srt_accept(s.listener, reinterpret_cast<sockaddr*>(&accept_addr), &accept_len);
    CHECK(s.accepted != SRT_INVALID_SOCK);
    CHECK(accept_addr.sin_addr.s_addr == caller_addr.sin_addr.s_addr);
    CHECK(accept_addr.sin_port == caller_addr.sin_port);
    CHECK(srt_getsockstate(s.caller) == SRTS_CONNECTED);
    CHECK(srt_getsockstate(s.accepted) == SRTS_CONNECTED);
    CHECK(SetOptInt(s.accepted, SRTO_RCVTIMEO, kTimeoutMs));

    std::printf("handshake: skip-induction connect/accept, no explicit bind\n");
    return true;
}

// (d) RTT seed read back before any data flows; (f) payload readback on both
// ends after negotiation.
bool PostConnectChecks(Session& s)
{
    SRT_TRACEBSTATS perf;
    CHECK(srt_bstats(s.caller, &perf, 0) == 0);
    CHECK(perf.msRTT == static_cast<double>(kSeedRttMs));

    int payload = 0;
    int optlen  = sizeof payload;
    CHECK(srt_getsockopt(s.caller, 0, SRTO_PAYLOADSIZE, &payload, &optlen) == 0);
    CHECK(payload == kPayloadSize);
    optlen = sizeof payload;
    CHECK(srt_getsockopt(s.accepted, 0, SRTO_PAYLOADSIZE, &payload, &optlen) == 0);
    CHECK(payload == kPayloadSize);

    std::printf("rtt seed: %.1f ms (pre-data); payload: %d / %d negotiated\n", perf.msRTT, kPayloadSize, kPayloadSize);
    return true;
}

bool SendAll(SRTSOCKET u)
{
    uint32_t words[kPayloadSize / 4];
    for (int i = 0; i < kNumMessages; ++i)
    {
        FillMessage(i, words);
        CHECK(srt_sendmsg(u, reinterpret_cast<const char*>(words), kPayloadSize, SRT_MSGTTL_INF, false) ==
              kPayloadSize);
    }
    return true;
}

void ReceiveAll(SRTSOCKET u, std::atomic<bool>& ok)
{
    uint32_t words[kPayloadSize / 4];
    char     buf[kPayloadSize];
    for (int i = 0; i < kNumMessages; ++i)
    {
        const int n = srt_recvmsg(u, buf, sizeof buf);
        if (n != kPayloadSize)
        {
            Fail("recvmsg " + std::to_string(i) + " returned " + std::to_string(n) + " [" + srt_getlasterror_str() +
                 "]");
            ok = false;
            return;
        }
        FillMessage(i, words);
        if (std::memcmp(buf, words, kPayloadSize) != 0)
        {
            Fail("message " + std::to_string(i) + " content mismatch");
            ok = false;
            return;
        }
    }
}

// (b) Byte-exact stream both directions of the wire; (c) the swallowed packet
// must be recovered, so the receiver still delivers everything in order.
bool RunData(Session& s)
{
    std::atomic<bool> rx_ok{true};
    std::thread       rx([&] { ReceiveAll(s.accepted, rx_ok); });
    const bool        tx_ok = SendAll(s.caller);
    rx.join();
    return tx_ok && rx_ok;
}

// (c) The loss was detected and recovered, not dropped: receiver-side loss
// and retransmit counters, sender-side NAK and retransmit counters, and no
// too-late drop on the receiver.
bool StatsChecks(Session& s)
{
    SRT_TRACEBSTATS rcv;
    CHECK(srt_bstats(s.accepted, &rcv, 0) == 0);
    CHECK(rcv.pktRcvLossTotal >= 1);
    CHECK(rcv.pktRcvRetrans >= 1);
    CHECK(rcv.pktRcvDropTotal == 0);
    CHECK(rcv.pktSentNAKTotal >= 1);

    SRT_TRACEBSTATS snd;
    CHECK(srt_bstats(s.caller, &snd, 0) == 0);
    CHECK(snd.pktRecvNAKTotal >= 1);
    CHECK(snd.pktRetransTotal >= 1);

    std::printf("loss recovery: rcvLoss=%d rcvRetrans=%d rcvDrop=%d | sndNAKrcvd=%d sndRetrans=%d\n",
                rcv.pktRcvLossTotal,
                rcv.pktRcvRetrans,
                rcv.pktRcvDropTotal,
                snd.pktRecvNAKTotal,
                snd.pktRetransTotal);
    return true;
}

} // namespace

int main()
{
    if (srt_startup() < 0)
    {
        Fail("srt_startup");
        return 1;
    }

    const int fds_before = CountSocketFds();

    // The label address the attached listener is bound to; the fake caller
    // address the accepted socket will report as its peer.
    const sockaddr_in listener_label = FillV4("10.213.255.254", 0);
    const sockaddr_in caller_addr    = FillV4("10.220.0.1", 5556);

    const auto t0 = std::chrono::steady_clock::now();

    bool ok = false;
    {
        Session session;
        ok = ConnectPair(session, listener_label, caller_addr) && PostConnectChecks(session) && RunData(session) &&
             StatsChecks(session);
    }

    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
    const int fds_after = CountSocketFds();
    if (fds_before < 0 || fds_after != fds_before)
    {
        Fail("socket fd count changed: before=" + std::to_string(fds_before) + " after=" + std::to_string(fds_after));
        ok = false;
    }

    srt_cleanup();
    if (!ok || !g_ok)
    {
        return 1;
    }

    std::printf("data: %d messages x %d bytes byte-identical in order\n", kNumMessages, kPayloadSize);
    std::printf("fds: %d before == %d after (zero OS sockets)\n", fds_before, fds_after);
    std::printf("elapsed: %lld ms\n", static_cast<long long>(elapsed_ms.count()));
    return 0;
}
