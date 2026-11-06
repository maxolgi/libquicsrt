// Protocol constants shared across the QuicSRT layers (WT transport, SRT
// sessions, public-config defaults). One home so the client library and
// its consumers (the gateway) cannot drift apart.

#ifndef QUICSRT_PROTO_CONSTANTS_H
#define QUICSRT_PROTO_CONSTANTS_H

#include <algorithm>
#include <cstddef>

namespace quicsrt
{

// SRT live payload: 6 x 188-byte MPEG-TS packets. Matches the Rust
// reference (crates/quicsrt/src/srt_sender.rs); the handshake negotiates
// min(gateway, client). Do not "fix" to 1316.
constexpr int kSrtPayloadSize = 1128;

// MPEG-TS packet size on the wire; the framing never splits one.
constexpr int kTsPacketSize = 188;

// Largest WT datagram payload queued: it must fit next to the context
// prefix inside a datagram frame on a minimum-MTU (1200-byte) packet.
// Distinct from the >= 1200-byte WT datagram transport-support
// requirement - one bounds the payload we queue, the other is what the
// path must carry.
constexpr std::size_t kMaxDatagramSize = 1184;

// WT datagram queue depth (per client session); a full queue drops the
// datagram whole and counts it - UDP-like loss SRT retransmits.
constexpr std::size_t kMaxQueuedDatagrams = 256;

// Receive-role buffer depth in packets, mirroring the browser peer
// (crates/srt-wasm/src/lib.rs new_inner: latency_ms * 20_000 / 1000 * 2
// packets, clamped to [8192, 320000], applied to both its buffers): enough
// for the TSBPD window at 20 packets per millisecond. This is RECEIVER
// sizing (the subscriber, and the control-only receive halves) - the
// data-sender window is SenderBufferPackets below; the split is
// deliberate, do not reunify them.
constexpr int BufferPackets(int latency_ms)
{
    const long long needed = static_cast<long long>(latency_ms) * 40;
    return static_cast<int>(std::clamp(needed, 8192LL, 320000LL));
}

// Send-role buffer depth in packets: the retransmit/flow-control window a
// data sender may hold unACKed. Deliberately NOT the wasm mirror: the
// Rust srt-wasm type is the browser LISTENER (its send half carries only
// SRT control traffic), and its 8192-packet floor exists for the browser
// TSBPD receive window and wasm memory - on SRTO_SNDBUF it is a ~12 MB
// retransmit window that swallows an unpaced push whole and masks reverse-
// path ACK starvation. The Rust native sender (crates/quicsrt/src/
// srt_sender.rs SrtConfig) already sizes its buffers independently of that
// formula. Here the transport is virtual with ~1 ms RTT (a BDP of tens of
// packets), so latency_ms * 8 (8 packets/ms: one TSBPD span in flight at a
// ~72 Mbps sustained push) with a 1024-packet floor (~1.5 MB at 1472
// bytes/packet) is a generously oversized window that still backpressures
// a whole-stream burst onto the ACK clock. Same 320000 ceiling as the
// mirror.
constexpr int SenderBufferPackets(int latency_ms)
{
    const long long needed = static_cast<long long>(latency_ms) * 8;
    return static_cast<int>(std::clamp(needed, 1024LL, 320000LL));
}

// The initial-RTT seed (SRT_VirtualTransportConfig.initial_rtt_ms handed
// to srt_bind_virtual): an explicitly configured initial_rtt_ms (> 0)
// overrides the transport-measured RTT, passing through the same 1..30000
// ms bounds the measured seed is clamped to (the public config validation
// already guarantees them). measured_ms arrives already clamped; 0 =
// unmeasured (leave srt's default estimator). One home so the subscriber
// and publisher listener halves cannot drift apart.
inline int InitialRttSeedMs(int configured_ms, int measured_ms)
{
    if (configured_ms > 0)
    {
        return static_cast<int>(std::clamp<long long>(configured_ms, 1, 30000));
    }
    return measured_ms;
}

} // namespace quicsrt

#endif // QUICSRT_PROTO_CONSTANTS_H
