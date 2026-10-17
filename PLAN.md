# Plan: ReceiverCore publish path (send_message)

Status: DRAFT. Consumer plan: `~/maxolgi/WebSRT-cpp/websrt-wasm-cpp/PLAN.md`
(M6 consumes this deliverable). Retire this doc when the send path is
merged, pushed, and the consumer pin is bumped.

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

## Semantics (fill in during step 1 — delete this note when done)

- chunking / size limit per call:
- actions emitted by send_message:
- poll() send-side surfacing:
- backpressure / too-late behavior:
- stats mapping:
