// publisher_starve_test: the publisher's SRT sender must ride out a
// transient blackout of the REVERSE path (the SRT control traffic - ACKs
// and keepalives - coming back from the gateway) without expiring its
// socket.
//
// The live-gateway evidence this locks in: an unpaced push (ffmpeg without
// -re) floods the datagram medium enough that the peer's control datagrams
// are lost for seconds while the peer received every media packet. The
// stock 5 s peer-idle budget then breaks the sender's socket mid-push -
// silently, because live-mode too-late drops swallow the unACKed tail -
// and the gateway reaps the publisher session. The publisher's sockets
// carry a 15 s peer-idle budget (publisher.cpp kPeerIdleTimeoutMs), so a
// multi-second blackout that lifts is survived; a permanent blackout still
// expires the socket (a genuinely dead peer must be detected).
//
// Shape: the M0 loopback pair (skip-induction caller over virtual
// transports) with the ROLES OF THE LIVE TOPOLOGY - the caller is the
// gateway (receiver), the listener/accepted socket is the client (sender,
// blasting unpaced). The caller->listener wire (the ACK path) is blacked
// out for a configurable time.
//
//   1. TRANSIENT BLACKOUT (8 s, past the stock 5 s budget, inside the 15 s
//      one): after the blast both sockets stay connected, and a post-lift
//      write still flows to the receiver.
//   2. PERMANENT BLACKOUT: the accepted (sender) socket is reported broken
//      (peer-idle expiry) - dead peers must still be detected.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>

#include "srt.h"
#include "srttransport.h"

// The fork's internal C++ bind path (conclusion_first; see core.h).
#include "core.h"

namespace
{

constexpr int kPayload = 1128; // 6 x 188-byte TS packets - the QuicSRT constant
constexpr int kMsgs    = 4000;
constexpr int kPeerIdleMs = 15000; // publisher.cpp kPeerIdleTimeoutMs

std::atomic<bool> g_ok{true};

void Fail(const std::string& what)
{
    g_ok = false;
    std::fprintf(stderr, "publisher_starve_test: FAIL: %s\n", what.c_str());
}

bool SetOptInt(SRTSOCKET u, SRT_SOCKOPT opt, int value)
{
    return srt_setsockopt(u, 0, opt, &value, sizeof value) != SRT_ERROR;
}

// In-memory wire with a blackout: after `pass_first` datagrams everything
// is dropped (returned as sent - one-way loss), until (optionally) lifted.
struct Wire
{
    srt::SRTTransportQueue* peer       = nullptr;
    sockaddr_storage        src        = {};
    socklen_t               srclen     = 0;
    std::atomic<int>        pass_first{-1};
    std::atomic<int>        passed{0};
    std::atomic<int>        dropped{0};

    int Send(const char* data, int len)
    {
        if (pass_first.load() >= 0 && passed.load() >= pass_first.load())
        {
            dropped.fetch_add(1);
            return len; // swallowed: as if lost on the way
        }
        passed.fetch_add(1);
        if (peer->push(data, static_cast<size_t>(len), reinterpret_cast<const sockaddr*>(&src), srclen) == -1)
        {
            return -1;
        }
        return len;
    }
};

int WireSendThunk(void* opaq, const char* data, int len, const struct sockaddr*, int)
{
    return static_cast<Wire*>(opaq)->Send(data, len);
}

void FillWire(Wire& w, const sockaddr_in& sa)
{
    std::memcpy(&w.src, &sa, sizeof sa);
    w.srclen = sizeof sa;
}

sockaddr_in FillV4(const char* ip, std::uint16_t port)
{
    sockaddr_in sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(port);
    sa.sin_addr.s_addr = inet_addr(ip);
    return sa;
}

struct Pair
{
    SRTSOCKET listener = SRT_INVALID_SOCK;
    SRTSOCKET caller   = SRT_INVALID_SOCK;
    SRTSOCKET accepted = SRT_INVALID_SOCK;
    Wire        caller_wire;   // caller -> listener (the ACK path)
    Wire        listener_wire; // listener -> caller (the media path)
    srt::SRTTransportQueue caller_queue{&WireSendThunk, &caller_wire};
    srt::SRTTransportQueue listener_queue{&WireSendThunk, &listener_wire};

    ~Pair()
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
        caller_queue.detach();
        listener_queue.detach();
    }
};

bool Connect(Pair& p, const sockaddr_in& listener_label, const sockaddr_in& caller_label)
{
    p.caller_wire.peer   = &p.listener_queue;
    p.listener_wire.peer = &p.caller_queue;
    FillWire(p.caller_wire, caller_label);
    FillWire(p.listener_wire, listener_label);

    p.listener = srt_create_socket();
    if (!SetOptInt(p.listener, SRTO_TRANSTYPE, SRTT_LIVE) || !SetOptInt(p.listener, SRTO_PAYLOADSIZE, kPayload) ||
        !SetOptInt(p.listener, SRTO_LATENCY, 120) || !SetOptInt(p.listener, SRTO_PEERIDLETIMEO, kPeerIdleMs))
    {
        return false;
    }
    {
        const SRT_VirtualTransportConfig cfg = p.listener_queue.getConfig(0);
        if (srt::CUDT::bind_virtual(p.listener, (const sockaddr*)&listener_label, (int)sizeof listener_label, &cfg,
                                    /*conclusion_first=*/true) == SRT_ERROR)
        {
            return false;
        }
    }
    if (srt_listen(p.listener, 5) == SRT_ERROR)
    {
        return false;
    }

    p.caller = srt_create_socket();
    if (!SetOptInt(p.caller, SRTO_TRANSTYPE, SRTT_LIVE) || !SetOptInt(p.caller, SRTO_PAYLOADSIZE, kPayload) ||
        !SetOptInt(p.caller, SRTO_LATENCY, 120) || !SetOptInt(p.caller, SRTO_RCVSYN, 0))
    {
        return false;
    }
    {
        const SRT_VirtualTransportConfig cfg = p.caller_queue.getConfig(1);
        if (srt::CUDT::bind_virtual(p.caller, (const sockaddr*)&caller_label, (int)sizeof caller_label, &cfg,
                                    /*conclusion_first=*/true) == SRT_ERROR)
        {
            return false;
        }
    }
    if (srt_connect(p.caller, (const sockaddr*)&listener_label, sizeof listener_label) == SRT_ERROR)
    {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (srt_getsockstate(p.caller) == SRTS_CONNECTING && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    if (srt_getsockstate(p.caller) != SRTS_CONNECTED)
    {
        return false;
    }
    sockaddr_in peer{};
    int         peer_len = sizeof peer;
    p.accepted           = srt_accept(p.listener, (sockaddr*)&peer, &peer_len);
    if (p.accepted == SRT_INVALID_SOCK)
    {
        return false;
    }
    // The publisher session re-applies the peer-idle budget post-accept.
    SetOptInt(p.accepted, SRTO_PEERIDLETIMEO, kPeerIdleMs);
    return true;
}

// Blast kMsgs messages from the accepted (client-role) socket, unpaced.
void Blast(Pair& p, std::atomic<int>& sent)
{
    std::vector<char> msg(kPayload);
    for (int i = 0; i < kMsgs; ++i)
    {
        std::snprintf(msg.data(), msg.size(), "starve %d", i);
        if (srt_sendmsg2(p.accepted, msg.data(), kPayload, nullptr) != kPayload)
        {
            return; // the socket died (expected only under a permanent blackout)
        }
        sent.fetch_add(1);
    }
}

// Drain the caller (gateway-role) receiver.
void Pump(Pair& p, std::atomic<int>& received, std::atomic<bool>& stop)
{
    std::vector<char> buf(1024 * 1024);
    while (!stop.load())
    {
        const int n = srt_recvmsg(p.caller, buf.data(), (int)buf.size());
        if (n > 0)
        {
            received.fetch_add(1);
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
}

// Returns true when both sockets stayed connected for `watch_ms`. A
// positive `blackout_lift_ms` lifts the reverse-path blackout that far in.
bool StayConnected(Pair& p, int watch_ms, int blackout_lift_ms, int& lift_at)
{
    const auto start   = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds{watch_ms};
    bool       lifted   = blackout_lift_ms <= 0;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const int elapsed = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();
        if (!lifted && blackout_lift_ms > 0 && elapsed >= blackout_lift_ms)
        {
            lifted = true;
            lift_at = p.caller_wire.dropped.load();
            p.caller_wire.pass_first.store(-1); // lift the blackout
        }
        if (srt_getsockstate(p.accepted) == SRTS_BROKEN || srt_getsockstate(p.caller) == SRTS_BROKEN)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    return true;
}

} // namespace

int main()
{
    const auto t0 = std::chrono::steady_clock::now();
    const auto listener_label = FillV4("10.213.255.254", 0);
    const auto caller_label   = FillV4("10.220.0.1", 5556);

    // ---- 1. transient blackout: survived ----
    {
        Pair p;
        if (!Connect(p, listener_label, caller_label))
        {
            Fail("transient: pair connect failed");
            return 1;
        }
        p.caller_wire.pass_first.store(5); // a few control datagrams, then blackout

        std::atomic<int>  sent{0};
        std::atomic<int>  received{0};
        std::atomic<bool> pump_stop{false};
        std::thread       blaster([&] { Blast(p, sent); });
        std::thread       pumper([&] { Pump(p, received, pump_stop); });

        // The blast outruns the receiver's release for a moment; wait for
        // the full blast to be accepted, then hold the connection through
        // an 8 s blackout (past the stock 5 s budget, inside the 15 s one).
        const auto sent_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (sent.load() < kMsgs && std::chrono::steady_clock::now() < sent_deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        int lift_at = -1;
        const bool stayed = StayConnected(p, 9000, 8000, lift_at);
        pump_stop.store(true);
        blaster.join();
        pumper.join();

        if (!stayed)
        {
            Fail("transient: a socket broke during/after an 8 s reverse-path blackout");
        }
        if (srt_getsockstate(p.accepted) != SRTS_CONNECTED)
        {
            Fail("transient: the sender socket is not connected after the blackout lifted");
        }
        std::fprintf(stderr,
                     "publisher_starve_test: transient blackout: sent=%d received=%d dropped-then-lifted=%d\n",
                     sent.load(),
                     received.load(),
                     lift_at);
    }

    // ---- 2. permanent blackout: the sender still expires ----
    {
        Pair p;
        if (!Connect(p, listener_label, caller_label))
        {
            Fail("permanent: pair connect failed");
            return 1;
        }
        p.caller_wire.pass_first.store(5);

        std::atomic<int>  sent{0};
        std::atomic<int>  received{0};
        std::atomic<bool> pump_stop{false};
        std::thread       blaster([&] { Blast(p, sent); });
        std::thread       pumper([&] { Pump(p, received, pump_stop); });

        // A dead reverse path forever: the sender's peer-idle budget
        // (15 s) must still expire the socket.
        int dummy = 0;
        const bool stayed = StayConnected(p, 20000, 0, dummy);
        pump_stop.store(true);
        blaster.join();
        pumper.join();

        if (stayed)
        {
            Fail("permanent: the sender socket survived a permanent reverse-path blackout");
        }
        std::fprintf(stderr, "publisher_starve_test: permanent blackout: sender expired as expected\n");
    }

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
    if (!g_ok)
    {
        std::fprintf(stderr, "publisher_starve_test: FAILURES\n");
        return 1;
    }
    std::printf("publisher_starve_test: OK (%lld ms)\n", static_cast<long long>(ms.count()));
    return 0;
}
