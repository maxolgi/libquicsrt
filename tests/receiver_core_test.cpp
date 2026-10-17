// receiver_core_test: the WT-free ReceiverCore driven end to end against
// a raw forked-srt initiator over an in-memory wire pair (the M0 harness
// pattern with the core as the listener half). Gate criteria here: the
// SRTO_INITIAL_RTT seed selection - an explicit params.initial_rtt_ms
// overrides the transport-measured RTT the driver passes to Start, and 0
// keeps the measured seed. The observable is the accepted socket's
// pre-data msRTT (the fork seeds the estimator from the option before
// the first live ACK sample - the same readback M0 does on its caller).
// The pure selection units (InitialRttSeedMs) cover the publisher half's
// arithmetic too: its socket application needs a live WebTransport
// session, but both halves share the one helper.
//
// Plain checks (not assert) so the test stays meaningful in Release.

#include <arpa/inet.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "proto/constants.h"
#include "srt_session/receiver_core.h"

namespace
{

constexpr int kTimeoutMs = 5000; // fail fast instead of hanging

std::atomic<bool> g_ok{true};

void Fail(const std::string& what)
{
    g_ok = false;
    std::fprintf(stderr, "receiver_core_test: FAIL: %s\n", what.c_str());
}

int g_checks = 0;

// Plain checks (not assert) so the test stays meaningful in Release.
template <typename Cond>
void Check(const Cond& cond, const std::string& what)
{
    ++g_checks;
    if (!cond())
    {
        Fail(what);
    }
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

// ---- (1) seed-selection units (shared with the publisher half) ----

void SelectionUnits()
{
    Check([] { return websrt::InitialRttSeedMs(0, 3) == 3; }, "unset keeps the measured seed");
    Check([] { return websrt::InitialRttSeedMs(0, 0) == 0; }, "unset keeps unmeasured at 0");
    Check([] { return websrt::InitialRttSeedMs(500, 3) == 500; }, "explicit overrides the measured seed");
    Check([] { return websrt::InitialRttSeedMs(500, 0) == 500; }, "explicit overrides unmeasured");
    Check([] { return websrt::InitialRttSeedMs(40000, 3) == 30000; }, "explicit passes the 1..30000 bounds");
}

// ---- (2) session-level seed readback ----

struct Harness
{
    websrt::ReceiverCore* core           = nullptr;
    SRTVirtualTransport*  caller_vt      = nullptr;
    sockaddr_in           listener_label = {};
};

// caller -> core: the raw socket's send callback hands the datagram to
// the core's inject (the core owns its own transport and labels the peer
// itself).
int CallerSend(void* opaq, const char* data, int len, const struct sockaddr* /*dst*/, int /*dstlen*/)
{
    static_cast<Harness*>(opaq)->core->InjectDatagram(reinterpret_cast<const std::uint8_t*>(data),
                                                      static_cast<std::size_t>(len));
    return len;
}

// core -> caller: the core's sink rides the raw socket's transport,
// sourced from the label the caller connected to.
int CoreSink(void* opaq, const char* data, int len)
{
    Harness* h = static_cast<Harness*>(opaq);
    return srt_virtual_transport_inject(h->caller_vt,
                                        data,
                                        len,
                                        reinterpret_cast<const sockaddr*>(&h->listener_label),
                                        static_cast<int>(sizeof h->listener_label)) == -1
               ? -1
               : len;
}

// Runs one handshake with the given configured/measured RTT pair and
// returns the accepted socket's pre-data msRTT (the applied seed), or a
// negative value after reporting the failure.
double AcceptedSeedMs(int configured_ms, int measured_ms)
{
    Harness h;
    // The fork's synthesized bind-less listener label (M0's constant);
    // the caller's fake source label is the core's own peer_label_.
    h.listener_label = FillV4("10.213.255.254", 0);

    websrt::ReceiverCoreParams params;
    params.initial_rtt_ms = configured_ms;

    websrt::ReceiverCore core(params, &CoreSink, &h);
    h.core = &core;

    std::string error;
    if (!core.Start(measured_ms, &error))
    {
        Fail("core Start: " + error);
        return -1.0;
    }

    SRTSOCKET caller = srt_create_socket();
    if (caller == SRT_INVALID_SOCK)
    {
        Fail("srt_create_socket");
        core.Finalize();
        return -1.0;
    }
    bool ok     = true;
    auto SetOpt = [&ok](SRTSOCKET u, SRT_SOCKOPT opt, int value)
    { ok = ok && srt_setsockopt(u, 0, opt, &value, sizeof value) != SRT_ERROR; };
    SetOpt(caller, SRTO_SKIP_INDUCTION, 1);
    SetOpt(caller, SRTO_PAYLOADSIZE, 1128);
    SetOpt(caller, SRTO_LATENCY, 120);
    h.caller_vt = srt_attach_virtual_transport(caller, CallerSend, &h);
    ok          = ok && h.caller_vt != nullptr;
    if (!ok)
    {
        Fail("caller socket setup");
        srt_close(caller);
        core.Finalize();
        return -1.0;
    }

    if (srt_connect(caller, reinterpret_cast<const sockaddr*>(&h.listener_label), sizeof h.listener_label) == SRT_ERROR)
    {
        Fail(std::string("srt_connect: ") + srt_getlasterror_str());
        srt_close(caller);
        srt_virtual_transport_release(h.caller_vt);
        core.Finalize();
        return -1.0;
    }

    double ms_rtt = -1.0;
    if (!core.WaitEstablished(kTimeoutMs, &error))
    {
        Fail("core WaitEstablished: " + (error.empty() ? "timeout" : error));
    }
    else
    {
        SRT_TRACEBSTATS st;
        if (!core.Stats(&st))
        {
            Fail("core Stats: no connected socket");
        }
        else
        {
            ms_rtt = st.msRTT;
        }
    }

    // Teardown in the M0 order: caller socket first (its shutdown rides
    // InjectDatagram into the still-open core), then the core's Finalize
    // (closes the core's sockets through the sink into the caller's
    // still-live transport handle), the caller transport released last.
    srt_close(caller);
    core.Finalize();
    srt_virtual_transport_release(h.caller_vt);
    return ms_rtt;
}

void SessionSeed()
{
    // 500 ms is far above any in-process handshake time, so a hit cannot
    // be a live sample; the measured 3 stands in for a loopback QUIC RTT.
    const double explicit_ms = AcceptedSeedMs(500, 3);
    if (explicit_ms >= 0.0)
    {
        Check([&] { return explicit_ms == 500.0; },
              "explicit initial_rtt_ms=500 overrides the measured seed (got " + std::to_string(explicit_ms) + ")");
    }

    const double derived_ms = AcceptedSeedMs(0, 3);
    if (derived_ms >= 0.0)
    {
        Check([&] { return derived_ms == 3.0; },
              "initial_rtt_ms=0 keeps the measured seed 3 (got " + std::to_string(derived_ms) + ")");
    }
}

} // namespace

int main()
{
    if (srt_startup() < 0)
    {
        Fail("srt_startup");
        return 1;
    }

    SelectionUnits();
    SessionSeed();

    srt_cleanup();
    if (!g_ok)
    {
        return 1;
    }
    std::printf("receiver_core_test: %d checks passed\n", g_checks);
    return 0;
}
