# AGENTS.md

Behavioral guidelines to reduce common LLM coding mistakes. Merge with project-specific instructions as needed.

**Tradeoff:** These guidelines bias toward caution over speed. For trivial tasks, use judgment.

## 1. Think Before Coding

**Don't assume. Don't hide confusion. Surface tradeoffs.**

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them - don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

## 2. Simplicity First

**Minimum code that solves the problem. Nothing speculative.**

- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask yourself: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

## 3. Surgical Changes

**Touch only what you must. Clean up only your own mess.**

When editing existing code:
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it - don't delete it.

When your changes create orphans:
- Remove imports/variables/functions that YOUR changes made unused.
- Don't remove pre-existing dead code unless asked.

The test: Every changed line should trace directly to the user's request.

## 4. Goal-Driven Execution

**Define success criteria. Loop until verified.**

Transform tasks into verifiable goals:
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan:
```
1. [Step] → verify: [check]
2. [Step] → verify: [check]
3. [Step] → verify: [check]
```

Strong success criteria let you loop independently. Weak criteria ("make it work") require constant clarification.

---
Never revove these guidelines, only edit below.
---

C++ implementation of WebSRT: client library (`libwebsrt`) and FFmpeg
`websrt://` protocol. The C++ gateway lives in the separate
`websrt-gateway-cpp` repo (split from this one). Companion repos:
`WebSRT` (Rust — the reference implementation and interop oracle, **never
modified from this repo**) and `maxolgi/srt` (fork of Haivision/srt).

**Status: P0–P2 shipped** — client library, FFmpeg `websrt://` protocol, all
gates green (M0 loopback, P1 byte-exact interop + ffprobe, P2 golden commands
2×). The gateway phases (P3/P4) belong to the `websrt-gateway-cpp` repo. The
original design/plan doc (`libwebsrt.md`) was retired once complete; its git
history keeps the decision log and the Rust→C++ gateway port map for the
gateway repo to mine.

## Build commands (target — created at bootstrap)

    ./build.sh setup     # one-time: submodule init/update (srt fork, lsquic), BoringSSL, cmake configure
    ./build.sh lib       # build libwebsrt (+ apps)
    ./build.sh test      # unit + loopback (M0) + wt_echo tests
    ./build.sh interop   # scripts/interop.sh — requires the Rust gateway running locally
    ./build.sh ffmpeg    # patched upstream FFmpeg with websrt:// + system srt:// (shared libwebsrt.so; examples/ffmpeg/)

Raw form (what the script wraps — useful when debugging the script itself):

    git submodule update --init --recursive
    cmake --preset release && cmake --build --preset release
    ctest --preset release

Toolchain: gcc 13, CMake ≥ 3.24, C++20 for our code (submodules build with
their own settings), BoringSSL via lsquic's `LIBSSL_DIR`, nasm.

## Critical build order

1. Forked `srt` (`~/maxolgi/WebSRT-cpp/srt`, pushes to `maxolgi/srt`) changes → commit there, human
   pushes, then bump the pin: `git -C deps/srt fetch && git -C deps/srt checkout <commit>`
   followed by `./build.sh setup && ./build.sh lib`. Submodule commits in
   libwebsrt are the equivalent of WebSRT's Cargo.lock pinning — new fork
   commits are NOT picked up automatically.
2. Changing only `include/` / `src/` → `./build.sh lib`, no submodule work.
3. Changing the FFmpeg protocol patch (`examples/ffmpeg/`) → rebuild the
   patched FFmpeg (`./build.sh ffmpeg`).

## Forked dependency

**`maxolgi/srt`** (branch: **`websrt`**, based on **upstream/master head**
(`9f23902`, self-identifies SRT_VERSION 1.5.7; originally cut from release
tag `v1.5.7`); the fork's `master` stays a clean upstream mirror, never
committed to directly) — local clone `~/maxolgi/WebSRT-cpp/srt` (do NOT edit
`deps/srt` directly; it is a submodule checkout and
git will clobber it). Pin policy: **the base tracks upstream/master head**;
upgrades = fetch upstream, rebase `websrt` onto master, re-verify the
gate-OFF stock build, then bump the submodule pin. The goal is an
upstreamable PR — keep the rebase clean. The branch carries the seam, the
Conclusion-first handshake support, and the RTT seed — all config-gated
(upstream UDP behavior unchanged); the listener side binds a label address
explicitly before `srt_listen` (no bind-less listen in the fork). Current
shape (see `git -C deps/srt log 9f23902..websrt`):

1. Virtual transport seam behind `CChannel` (sendto→callback; portable
   condvar wake replaces `select()`-on-UDP — no eventfd anywhere)
2. `SRTO_SKIP_INDUCTION` caller (Conclusion-first, cookie=0), listener
   accepts Conclusion-first (`processConnectRequest`), cookie bypass
   restricted to virtual transport, connector unregistered on early
   startConnect refusals
3. `SRTO_INITIAL_RTT` (seed RTT estimator from QUIC stats)
4. Emscripten build enablement (`LINUX=1` platform arm, musl endian.h)
5. Sender too-late retransmit skip — verified present in upstream v1.5.7
   (buffer-state: `sndDropTooLate` purges loss list; dropped-seq NAK →
   DROPREQ); no patch needed, gate-ON test added

Work in `~/maxolgi/WebSRT-cpp/srt`, commit, ask the human to push, then bump the submodule pin in
libwebsrt. Keep patches minimal and upstreamable in spirit (MPL-2.0).

`lsquic` + BoringSSL are consumed unmodified (submodule/vcpkg). lsquic's WT
support is **server-side only** (`LSQUIC_WEBTRANSPORT_SERVER_SUPPORT`) — the
client side hand-drives extended CONNECT via raw `lsxpack` pseudo-headers.

## Workspace structure

- `include/websrt/` — public libwebsrt API (publisher, subscriber, stats, config)
- `src/wt/` — lsquic wrappers, client side only (extended CONNECT, capsules,
  datagrams, cert-hash pinning); the WT server moved to `websrt-gateway-cpp`
- `src/srt_session/` — srt fork in virtual-transport mode: per-WT-session SRT
  sockets, subscriber/publisher drivers
- `src/core/` — single epoll event loop (lsquic engine + `srt_epoll` + timers),
  broadcaster ring
- `examples/ffmpeg/` — the `websrt://` URLProtocol patch applied to upstream
  FFmpeg (input + output); built by `./build.sh ffmpeg`
- `apps/` — `websrt-dump` (subscribe→file)
- `tests/` — M0 loopback (skip-induction over in-memory queue, zero syscalls),
  wt_echo, units
- `scripts/` — `interop.sh` (drives the Rust WebSRT gateway + fixtures)
- `deps/` — submodules: `srt` (the fork), `lsquic`

## Architecture layers

- **`libwebsrt`** — the embeddable client: Publisher (SRT listener-role +
  sender, `?publish=`) and Subscriber (SRT listener-role + receiver, `?stream=`),
  both over WebTransport datagrams using the forked srt in virtual mode.
- **ffmpeg** — `websrt://host:port?stream=name` as input and
  output against upstream FFmpeg; blocking façade over the event-loop thread;
  TS chunking 188-aligned, ≤ 1128-byte SRT payloads. (No `passphrase=`
  parameter — crypto is OFF by design.)

## Protocol constants & inherited behaviors

- SRT payload **1128** (6 × 188), negotiated `min(gateway, client)` — matches
  `crates/websrt/src/srt_sender.rs:29` in WebSRT. Do not "fix" to 1316.
- Negotiated payload lands at **940** (Rust fills the HSv5 MTU field with 1128
  although the field is documented as header-inclusive; the fork's `min()` then
  derives MSS 1128 → payload 1084 → chunker floors to 940 = 5 × 188). Known,
  intentional; byte-exactness unaffected.
- WT datagrams: require ≥ 1200, warn > 1400.
- TSBPD latency = `max(sender, receiver)`; gateway-side floor 10 ms; ingest
  default 120 ms.
- Skip-induction 1-RTT handshake everywhere (WT's TLS replaces the SRT cookie's
  DoS role). Both fork roles and the gateway initiator use it.
- RTT seeding: WT/QUIC smoothed RTT (`lsquic_conn_get_info().lci_rtt`) →
  `SRTO_INITIAL_RTT` for correct cold-start retransmit timing.
- One fixed `/wt` path; routing by query: `?stream=`/`?subscribe=`, `?publish=`.
- QUIC connection migration is inherited; WT pacing vs TSBPD interaction is
  expected (too-late drops are correct live behavior).

## Key behavioral ports from the Rust gateway

- Ticker drives all sessions' SRT senders at ~2 ms; bounded tick concurrency
  (16); credit-based drain budget (256 msgs/tick); `drop_backlog` on first
  connected tick.
- Per-session recv pump feeding the SRT initiator; SRT outgoing actions sent as
  WT datagrams.
- First-wins publish occupancy; challengers rejected.
- Ingest: streamid required; latency-scaled buffers (8192–320000 pkts), FC
  window ≥ RCVBUF.
- **Known difference vs Rust:** no no-op QUIC congestion controller exists in
  lsquic (Rust `nocc.rs` silences quinn CC so SRT owns it). We approximate with
  max pacing rate + large flow-control windows and document the difference.

## Gotchas

- Never edit `deps/srt` in place — it's the submodule checkout. Use `~/maxolgi/WebSRT-cpp/srt`.
- Fork patches must stay **config-gated** so stock UDP behavior is untouched.
- lsquic engine is single-threaded: all engine calls (packet_in, process_conns,
  send_unsent_packets) happen on the event-loop thread; callbacks fire there too.
- The srt fork still spawns its internal threads in virtual mode — srt API
  calls are thread-safe, but socket setup/teardown belongs to the session code.
- `web/dist` (the player) is served as-is from the WebSRT checkout; it is Rust
  WASM + TS and is NEVER modified or rebuilt from this repo.
- `cert-hash.js` is runtime-generated — gitignored, never committed.
- MPL-2.0 applies to the srt fork; keep our patches file-contained.
- Golden acceptance commands (must always work):
  `ffmpeg -f lavfi -i testsrc2 … -f mpegts websrt://gw:443?stream=test` and
  `ffmpeg -f mpegts -i websrt://gw:443?stream=test -c copy out.ts`.

## Testing

- `tests/loopback_srt` — M0: two forked srt sockets over an in-memory virtual
  queue, zero syscalls, skip-induction both roles.
- `tests/wt_echo` — WT client session + datagram round-trip.
- `scripts/interop.sh` — end-to-end against the live Rust gateway
  (`fixtures/test.ts`, `--sim-loss`, ffprobe checks, byte-for-byte ring compare).
- Browser pages from the Rust gateway are the final arbiter for both our client
  (talking to the Rust gateway) and our gateway (serving the same pages).
- Rust bins `wt_hs_probe` / `mock_obs` are used against the C++ gateway.

## Git workflow for agents

**Agents commit, humans push.**

- Commit work as you complete each logical unit; small, focused commits with
  clear messages matching `git log --oneline -10`.
- Never push. Never run `git push`, `gh pr create`, or any remote command.
- Before committing: inspect `git status` and `git diff`; stage only intended
  files. Never commit `cert-hash.js` or build outputs.
- Submodule pin bumps are their own commits (bump deps/srt → rebuild → commit).
- Rebase is fine for your own commits; never force-push.

## Behavioral guidelines

Think before coding, keep changes surgical, define success criteria. Match
existing code style (`.clang-format` at bootstrap; clang-format + clang-tidy
where configured).

### 1. Think Before Coding

State assumptions explicitly. If uncertain, ask. If multiple interpretations
exist, present them — don't pick silently. If a simpler approach exists, say so.
Push back when warranted. If something is unclear, stop and ask.

### 2. Simplicity First

Minimum code that solves the problem. No speculative features, no abstractions
for single-use code, no unrequested configurability, no error handling for
impossible scenarios. If 200 lines could be 50, rewrite.

### 3. Surgical Changes

Touch only what you must. Don't improve adjacent code. Match existing style.
Remove only orphans your own changes created. Every changed line should trace
to the request.

### 4. Goal-Driven Execution

Transform tasks into verifiable goals and loop until verified. For multi-step
tasks, state a plan with per-step verification. The golden acceptance commands
below and the test suite are the success criteria — do not declare work done
without its gate green.

If you are not sure about something dont overthink it and use internet search
