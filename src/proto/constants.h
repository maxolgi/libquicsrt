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

// Buffer depth in packets, mirroring the browser peers
// (crates/srt-wasm/src/lib.rs new_inner): enough for the TSBPD window at
// 20 packets per millisecond, both directions, clamped to the ingest range.
constexpr int BufferPackets(int latency_ms)
{
    const long long needed = static_cast<long long>(latency_ms) * 40;
    return static_cast<int>(std::clamp(needed, 8192LL, 320000LL));
}

// The SRTO_INITIAL_RTT seed: an explicitly configured initial_rtt_ms (> 0)
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
