// HSv5 negotiated-payload readback: the effective SRT payload size of an
// accepted socket, derived after the handshake. Exported via the src/
// interface so the gateway's ingest side frames to the same size.

#ifndef WEBSRT_SRT_SESSION_NEGOTIATE_H
#define WEBSRT_SRT_SESSION_NEGOTIATE_H

// The fork's gated API (build with SRT_ENABLE_VIRTUAL_TRANSPORT defined).
#include "srt.h"

namespace websrt
{

// The handshake negotiates min(gateway, listener) payload: the framing
// must use the negotiated size, not the configured one (M0's
// negotiated-payload readback). Two readbacks matter:
//   - SRTO_PAYLOADSIZE reports the EXPECTED size (the configured value),
//     not the effective cap;
//   - the real cap is MSS - 44 (UDP+IP+SRT data headers), and the Rust
//     initiator (srt_sender.rs) fills the HSv5 MTU field with its payload
//     size (1128) although srt-protocol documents that field as "including
//     UDP/IP headers" - so the fork's min() negotiation lands MSS at 1128
//     and the effective cap at 1084. Flooring the cap to whole TS packets
//     (940 = 5 x 188) keeps the framing 188-aligned.
int NegotiatePayload(SRTSOCKET accepted, int configured);

} // namespace websrt

#endif // WEBSRT_SRT_SESSION_NEGOTIATE_H
