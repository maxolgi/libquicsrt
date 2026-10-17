# Plan: ReceiverCore publish path (send_message)

Status: Step 1 done (semantics below filled in from the oracle study);
implementation steps 2-5 pending.

## Goal

Give the WT-free `ReceiverCore` (`src/srt_session/receiver_core.{h,cpp}`)
the message send path that the Rust oracle `SrtReceiver.send_message`
(`WebSRT/crates/srt-wasm/src/lib.rs:391`) already has, so
websrt-wasm-cpp can publish from the browser through the same Emscripten
flavor it already builds (`WEBSRT_RECEIVER_ONLY=ON`).

**No new flavor, no EventLoop/epoll work.** In every browser session —
subscribe AND publish — the gateway is the SRT caller and the browser
engine is the listener (see the thread/role notes at the top of
`receiver_core.h`). The accepted listener socket already exists; the srt
fork (linked wholesale as `websrt::srt-deps`) carries the full sender
machinery: virtual-transport `VtSend` sink is wired, sender too-late
retransmit skip is verified (gate-ON test), and SRT's internal threads run
in virtual mode. `SRTO_SNDBUF` is already configured on the listener
(header documents it). Publishing only needs a send entry point.

Today's gap is explicit: `websrt-wasm-cpp/js/srt_engine.js` documents
"`sendMessage()` is NOT implemented: the underlying C ABI
(harness/receiver.h) is receive-only".

## Deliverables

1. `src/srt_session/receiver_core.{h,cpp}`:
   - `SendMessage(...)` mirroring the Rust oracle's semantics exactly
     (chunking/message boundaries against the negotiated payload — remember
     the effective 940-byte chunk that HSv5 negotiation produces — return
     codes for not-yet-handshaked/closed, behavior when the send buffer is
     full or a too-late drop occurs).
   - tx stats surfaced through the existing stats path (`txData txBytes
     txRetransmit txLoss txBuffered` — the Rust `SrtStats` fields the
     consumer's `stats_map` already mirrors; today they read zero).
   - Header comment updated: send path, which thread domain may call it,
     and any teardown-order consequences (the header's three-thread-domain
     rules stay authoritative).
2. `tests/receiver_core_test.cpp`: a loopback publish case — `ReceiverCore`
   listener + the existing in-memory peer caller; messages sent via
   `SendMessage` arrive at the peer byte-identical, in order; tx stats
   non-zero afterwards; send-before-handshake and send-after-close return
   the documented statuses.
3. CMake: expected to need nothing (receiver_core.cpp is already the only
   source of the receiver-only flavor). Only touch it if the send path
   genuinely needs a second file.

## Steps (each → verify, each its own commit)

1. Study the Rust oracle and write the semantics into this doc before
   coding: what `send_message` does per call (message mode? stream mode?
   pre-chunking?), size limits vs the 940/1128 negotiated payload, which
   `SrtAction`s it can emit, and what poll() surfaces on the send side →
   verify: semantics section below filled in and re-read against
   `crates/srt-wasm/src/lib.rs` (and the srt-rs fork's session send path
   if chunking lives there).
2. Implement `SendMessage` + tx stats → verify: `./build.sh lib &&
   ./build.sh test` green including the new loopback publish case.
3. Edge cases per the oracle: send before handshake complete, send after
   close, buffer-full backpressure → verify: unit tests cover each with
   the oracle's exact behavior.
4. Docs + commit → verify: golden acceptance commands still clean
   (`ffmpeg … -f mpegts websrt://gw:443?stream=test` and the `-c copy`
   playback direction).
5. Hand off: human pushes; websrt-wasm-cpp bumps `deps/libwebsrt` and
   runs its PLAN M6.

## Out of scope

WT layer (the consumer's harness supplies the transport), EventLoop/epoll,
gateway changes, crypto (OFF by design), any change to stock UDP behavior
of the srt fork.

## Gates

`./build.sh lib && ./build.sh test` green; receiver_core_test extended and
green; stock build (receiver-only OFF) still builds and passes — the send
path must not regress the full library. This repo's AGENTS.md build order
and pin-chain rules apply unchanged.

## Semantics (oracle study, step 1)

Oracle: `SrtReceiver.send_message(bytes, now_us)` — `WebSRT/crates/srt-wasm/src/lib.rs:390-406` ("lib.rs" below); the send machinery lives in the maxolgi/srt-rs fork (srt-protocol). C++ references: `publisher.cpp` = `src/srt_session/publisher.cpp`, `core.cpp` = `deps/srt/srtcore/core.cpp`.

- chunking / size limit per call: Oracle does NOT pre-chunk — the whole
  buffer goes down as ONE message (one message_number + timestamp per call,
  packets tagged FIRST/MIDDLE/LAST in srt-rs `sender/encapsulate.rs:25-39`),
  split purely by the negotiated `max_packet_size` (HSv5 `min()` = 1128 both
  sides; srt-rs subtracts no headers). No per-call limit, no 188-alignment
  at the protocol layer (whole-TS input is the muxer's contract upstream),
  and an empty buffer still emits one empty packet (unreachable in practice
  — the worker guards `tsBytes.length === 0`, stream-worker.ts:412).
  **C++ mapping:** C srt live+message API caps one packet per call
  (`srt_sendmsg2` > negotiated payload → `SRT_ELARGEMSG`, core.cpp:7205), so
  SendMessage pre-chunks per call: split at 940 (5×188) boundaries and flush
  any short remainder as a final short chunk in the SAME call — never
  carried across calls (the oracle keeps no cross-call send state). Each
  chunk is its own srt message (message boundaries differ from the oracle's
  single FIRST..LAST message; in-order wire bytes are identical and the
  Rust gateway reassembles either). Empty input → no-op.
- actions emitted by send_message: Oracle returns `Vec<SrtAction>` (kinds
  0 SendDatagram / 3 WaitForData / 4 Close possible from the post-send
  drain, lib.rs:53-54); not-handshaked AND closed both return an empty vec
  silently (lib.rs:404), distinguished only by probes
  `is_handshake_complete()`/`is_closed()`. **C++ mapping:** ReceiverCore
  has no action channel — srt's internal threads pace and emit via VtSend —
  so SendMessage returns int: bytes accepted (>0) or a `kSend*` status,
  with DISTINCT codes for not-handshaked / closed (a deliberate, documented
  deviation the Deliverables mandate; WaitEstablished/IsClosed remain the
  probes). Accepted bytes leave via VtSend later, exactly as the oracle's
  paced Output queue (SND-timer driven).
- poll() send-side surfacing: Oracle: none beyond SendDatagram — no
  buffer-space, too-late-drop, or send-error events exist (the srt-rs
  Action enum is ReleaseData|SendPacket|UpdateStatistics|WaitForData|Close;
  UpdateStatistics is swallowed internally, lib.rs:515-518; failures
  surface only as Close). **C++:** nothing to add to Read/poll, no new
  event kinds.
- backpressure / too-late behavior: Oracle: send buffer full → silent
  drop-OLDEST + tx_dropped_* stats only (srt-rs sender/buffer.rs:66-89);
  flow window → WaitForAck stall past 25600 unacked; sender too-late drop
  ungated at max(1.25×TSBPD, 1s) (buffer.rs:275-313). **C++ mapping:**
  srt_sendmsg2 BLOCKS on a full buffer — bound it per call with
  SRTO_SNDTIMEO (the Publisher::SendOne pattern, publisher.cpp:414-438):
  `SRT_ETIMEOUT` → kSendTimeout (the buffer-full answer), `SRT_ECONNLOST`/
  `SRT_ESCLOSED`/`SRT_ENOCONN` → kSendClosed, `SRT_ELARGEMSG` →
  kSendInvalidArg; Finalize's socket close wakes an in-flight send
  (SRT_ECONNLOST) so Stop stays prompt. Too-late drops happen INSIDE srt
  (`sndDropTooLate`, gate-ON verified) and surface as pktSndDrop stats,
  not return codes. No drop-oldest at the API: the C stack blocks instead
  — same live outcome on the wire, different caller signal (documented
  difference).
- stats mapping (exact, via SRT_TRACEBSTATS): txData ← pktSentTotal (incl.
  retransmits) · txBytes ← byteSentTotal (incl. 44 B IP+UDP+SRT headers) ·
  txRetransmit ← pktRetransTotal · txLoss ← pktSndLossTotal · txBuffered ←
  pktSndBuf (instantaneous, not a total — plain srt_bstats IIR-smooths the
  sndbuf fields, srt_bistats(..., instantaneous=1) reports raw; pick at
  implementation to keep the oracle's instantaneous semantics, cf.
  publisher.cpp StatsInstant). Pull-only getter (oracle refreshes on a 1 s
  internal timer, exposes getStats(); the consumer's srtw_get_stats /
  harness stats_map.h already carries all five tx fields at fixed offsets —
  no ABI change needed for stats).
