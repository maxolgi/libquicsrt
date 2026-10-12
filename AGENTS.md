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

C++ implementation of WebSRT: client library (`libwebsrt`), CLI gateway
(Rust-gateway parity), and FFmpeg `websrt://` protocol. Companion repos:
`WebSRT` (Rust — the reference implementation and interop oracle, **never
modified from this repo**) and `maxolgi/srt` (fork of Haivision/srt).

**Status: bootstrap pending.** The full design, decision log, phases, and gates
live in `websrt-cpp.md` (read it before implementing). This file defines the
working conventions once code lands.

## Build commands (target — created at bootstrap)

    ./build.sh setup     # one-time: submodule init/update (srt fork, lsquic), BoringSSL, cmake configure
    ./build.sh lib       # build libwebsrt (+ apps)
    ./build.sh gateway   # build the C++ gateway binary
    ./build.sh test      # unit + loopback (M0) + wt_echo tests
    ./build.sh interop   # scripts/interop.sh — requires the Rust gateway running locally
    ./build.sh ffmpeg    # builds patched upstream FFmpeg with the websrt protocol (examples/ffmpeg/)

Raw form (what the script wraps — useful when debugging the script itself):

    git submodule update --init --recursive
    cmake --preset release && cmake --build --preset release
    ctest --preset release

Toolchain: gcc 13, CMake ≥ 3.24, C++20 for our code (submodules build with
their own settings), BoringSSL via lsquic's `LIBSSL_DIR`, nasm.

## Critical build order

1. Forked `srt` (`~/maxolgi/srt`, pushes to `maxolgi/srt`) changes → commit there, human
   pushes, then bump the pin: `git -C deps/srt fetch && git -C deps/srt checkout <commit>`
   followed by `./build.sh setup && ./build.sh lib`. Submodule commits in
   websrt-cpp are the equivalent of WebSRT's Cargo.lock pinning — new fork
   commits are NOT picked up automatically.
2. Changing only `include/` / `src/` → `./build.sh lib`, no submodule work.
3. Changing `src/ffmpeg/` → rebuild the patched FFmpeg (`./build.sh ffmpeg`).
4. Changing `src/gateway/` → `./build.sh gateway` + `./build.sh restart`
   (production only, via supervisord).

## Forked dependency

**`maxolgi/srt`** (branch: **`websrt`**, cut from upstream **release tag
`v1.5.7`** — commit `899348d8318eb9a3c5a5b6ec43c4a1114288773a`; the fork's
`master` stays a clean upstream mirror, never committed to directly) — local
clone `~/maxolgi/srt` (do NOT edit `deps/srt` directly; it is a submodule checkout and
git will clobber it). Pin policy: **releases only, never upstream master**
(FFmpeg-supported line, fewer rebase surprises). Upgrades = fetch the new
release tag from Haivision upstream, rebase `websrt` onto it, re-verify the
gate-OFF build, then bump the submodule pin. Six config-gated patches (upstream
UDP behavior unchanged; see `websrt-cpp.md` §"srt fork — patch list") — five
code patches plus one verified-upstream no-op:

1. Virtual transport seam behind `CChannel` (sendto→callback, eventfd wake
   replaces `select()`-on-UDP)
2. Attached-socket mode (per-WT-session sockets without bind/listen)
3. `SRTO_SKIP_INDUCTION` caller (Conclusion-first, cookie=0)
4. Listener accepts Conclusion-first (`processConnectRequest`)
5. `SRTO_INITIAL_RTT` (seed RTT estimator from QUIC stats)
6. Sender too-late retransmit skip — verified present in upstream v1.5.7
   (buffer-state: `sndDropTooLate` purges loss list; dropped-seq NAK →
   DROPREQ); no patch needed, gate-ON test added

Work in `~/maxolgi/srt`, commit, ask the human to push, then bump the submodule pin in
websrt-cpp. Keep patches minimal and upstreamable in spirit (MPL-2.0).

`lsquic` + BoringSSL are consumed unmodified (submodule/vcpkg). lsquic's WT
support is **server-side only** (`LSQUIC_WEBTRANSPORT_SERVER_SUPPORT`) — the
client side hand-drives extended CONNECT via raw `lsxpack` pseudo-headers.

## Workspace structure

- `include/websrt/` — public libwebsrt API (publisher, subscriber, stats, config)
- `src/wt/` — lsquic wrappers: WT client (extended CONNECT, capsules, datagrams,
  cert-hash pinning) and WT server (lsquic's supported path)
- `src/srt_session/` — srt fork in virtual-transport mode: per-WT-session SRT
  sockets, subscriber/publisher drivers
- `src/core/` — single epoll event loop (lsquic engine + `srt_epoll` + timers),
  broadcaster ring
- `src/gateway/` — the gateway binary: accept loop, session/registry/ticker,
  stream registry, hooks, limits, certs, web server, UDP ingest (stock-mode libsrt)
- `src/ffmpeg/` — `websrt://` URLProtocol (input + output)
- `apps/` — `websrt-dump` (subscribe→file), `websrt-publish-file` (loop a .ts)
- `tests/` — M0 loopback (skip-induction over in-memory queue, zero syscalls),
  wt_echo, units
- `scripts/` — `interop.sh` (drives the Rust WebSRT gateway + fixtures),
  supervisord conf
- `deps/` — submodules: `srt` (the fork), `lsquic`

## Architecture layers

- **`libwebsrt`** — the embeddable client: Publisher (SRT listener-role +
  sender, `?publish=`) and Subscriber (SRT listener-role + receiver, `?stream=`),
  both over WebTransport datagrams using the forked srt in virtual mode.
- **gateway** — CLI-parity replacement of the Rust gateway (CLI-only, no GUI).
  lsquic WT server; per-browser-session SRT initiator (skip-induction
  Conclusion-first, cookie=0); broadcaster fanout (TS bytes never modified);
  SRT/UDP ingest via stock-mode libsrt (streamid = routing key, first-wins);
  cert modes + runtime `cert-hash.js`; static serving of the existing Rust
  player (`web/dist` — path configured, never rebuilt here); health endpoint;
  supervisord.
- **ffmpeg** — `websrt://host:port?stream=name[&passphrase=…]` as input and
  output against upstream FFmpeg; blocking façade over the event-loop thread;
  TS chunking 188-aligned, ≤ 1128-byte SRT payloads.

## Protocol constants & inherited behaviors

- SRT payload **1128** (6 × 188), negotiated `min(gateway, client)` — matches
  `crates/websrt/src/srt_sender.rs:29` in WebSRT. Do not "fix" to 1316.
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

## Runtime (gateway, production)

- Runs under supervisord: config `scripts/websrt-cpp.conf` → deploy like WebSRT's.
- Logs: `/var/log/websrt-cpp/`.
- Restart: `sudo supervisorctl restart websrt-cpp` after rebuild.
- On boot the gateway writes `cert-hash.js` next to the served web assets
  (hash for self-signed, null for mkcert).

## Cert modes

- `--cert-mode self` (default): self-signed ECDSA, **regenerated on every boot —
  never persisted**. `--cert-mode mkcert`: loads PEMs.
- Cert hash changes on every restart — browser must reload the page.

## Gotchas

- Never edit `deps/srt` in place — it's the submodule checkout. Use `~/maxolgi/srt`.
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
tasks, state a plan with per-step verification. Phases and gates in
`websrt-cpp.md` are the success criteria — do not declare a phase done without
its gate green.

If you are not sure about something dont overthink it and use internet search
