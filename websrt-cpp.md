# WebSRT-cpp — Master Plan

Status: planned (not yet implemented). This document is the complete, self-contained
plan for the implementation sessions. Companion repos: `WebSRT` (Rust, unmodified),
`maxolgi/srt` (fork of Haivision/srt — already forked on GitHub).

## Deliverables

1. **`libwebsrt`** — C++ client library: publish and subscribe WebSRT streams.
2. **C++ gateway** — full CLI-parity replacement of the Rust gateway. CLI-only
   (no GUI), production intent. Serves the existing browser player unchanged.
3. **FFmpeg integration** — `websrt://` URLProtocol, input **and** output,
   targeting upstream FFmpeg.

## Scope boundaries (locked decisions — do not re-litigate)

| Decision | Rationale |
|---|---|
| Browser side stays Rust WASM (srt-wasm, mpeg2ts-wasm, ts-muxer-wasm, web UI) | Wire-compatible; porting libsrt to WASM is research-grade (pthreads/UDP/select-bound); no benefit |
| No loopback-UDP shim between stock libsrt and WT | Rejected as "unreasonable hacking" — the clean path is the fork |
| No SRT egress feature in the Rust gateway | Rejected; C++ subscribes via native WebTransport instead |
| No MXL integration in this repo | Out of scope; composable later via ffmpeg (cbcrc/FFmpeg dmf-mxl) or gst-mxl-rs |
| Rust WebSRT repo: ZERO changes | The fork speaks skip-induction, so the existing gateway interops as-is |
| srt-core = fork of Haivision/srt (`maxolgi/srt`) | User intent: Haivision SRT; hand-written port rejected; stock-submodule-only rejected (fd-bound API, no injection point, Induction-mandatory listener) |
| Repo is C++ (sibling to WebSRT at `~/maxolgi/websrt-cpp`) | — |

## Architecture

```
                         ┌──────────────────────────────────────────────┐
                         │ websrt-cpp                                    │
                         │                                               │
  apps / ffmpeg ────────►│ libwebsrt   Publisher / Subscriber façade     │
                         │    │                                          │
                         │    ├─► wt: lsquic client/server               │
                         │    │     extended CONNECT + capsules +        │
                         │    │     datagrams + cert-hash pinning        │
                         │    │                                          │
                         │    └─► srt-session: srt fork in virtual mode  │
                         │          (skip-induction both roles)          │
                         │                                               │
                         │ gateway: lsquic WT server + srt fork          │
                         │  initiator + broadcaster fanout + stock-mode  │
                         │  libsrt UDP ingest (same submodule)           │
                         │  + static player serving + cert-hash.js       │
                         └──────────────────────────────────────────────┘
```

- **SRT core**: fork `Haivision/srt` → `github.com/maxolgi/srt`, consumed as a git
  submodule pointing at the fork. Local clone `~/srt`, branch `websrt` cut from
  upstream **release tag `v1.5.7`** (commit `899348d8`) — **pin releases only,
  never upstream master** (FFmpeg-supported line, fewer rebase surprises). The
  fork's `master` stays a clean upstream mirror; upgrades rebase `websrt` onto a
  newer release tag, re-verify gate-OFF, then bump the pin.
  Same discipline as WebSRT's `~/srt-rs` / `~/mpeg2ts`: agents commit, human
  pushes, pin by commit hash. MPL-2.0 (file-level copyleft, fine for linking).
- **WebTransport**: lsquic 4.x + BoringSSL.
  - Client path (library): hand-driven — raw `lsxpack` pseudo-headers
    (`:method: CONNECT`, `:protocol: webtransport`, `:path: /wt?stream=…`),
    capsule layer on the session stream, client datagrams
    (`es_datagrams` + `on_datagram`/`on_dg_write` + `lsquic_conn_want_datagram_write`),
    cert pinning via `ea_verify_cert` (SHA-256 of DER), RTT via
    `lsquic_conn_get_info()` (`lci_rtt`) seeded into SRT.
  - Server path (gateway): lsquic's **supported** WT feature
    (`LSQUIC_WEBTRANSPORT_SERVER_SUPPORT`, `es_webtransport_server`).
  - Event model: single-threaded tick loop (`lsquic_engine_packet_in`,
    `lsquic_engine_process_conns`, `lsquic_engine_earliest_adv_tick`,
    `lsquic_engine_send_unsent_packets`) integrated with `srt_epoll` in one epoll core.

## Protocol constants to honor (from the Rust gateway)

- SRT payload **1128** bytes (6 × 188 TS packets), negotiated `min(gateway, client)`
  — see `crates/websrt/src/srt_sender.rs:29` (PAYLOAD_SIZE) for why 1128 not 1316
  (browser WT datagram cap ~1200).
- WT datagram size: require ≥ 1200, warn > 1400.
- TSBPD latency = `max(sender, receiver)`; gateway-side floor 10 ms; ingest default
  `--latency` 120 ms.
- Stream routing: one `/wt` path; `?stream=` / `?subscribe=` (view), `?publish=`
  (browser/C++ publish); OBS ingest derives name from SRT `?streamid=`.
- First-wins publish occupancy; TS bytes never modified server-side.
- Cert modes: self-signed ECDSA **regenerated every boot, never persisted**;
  mkcert PEM mode; runtime `cert-hash.js` emission (hash for self-signed, null
  for mkcert) — the C++ gateway must reproduce this.

## srt fork — patch list (all config-gated; upstream UDP behavior unchanged)

| # | Patch | Primary files |
|---|---|---|
| 1 | Virtual transport seam: injectable interface behind `CChannel`; `sendto` → callback; inbound queue + eventfd wake replaces `select()`-on-UDP | `srtcore/channel.{h,cpp}`, `queue.cpp` |
| 2 | Attached-socket mode: per-WT-session sockets without `bind`/`listen` | `api.cpp`, `core.cpp` |
| 3 | `SRTO_SKIP_INDUCTION` caller: Conclusion-first, cookie = 0 | `core.cpp`, `handshake.cpp` |
| 4 | Listener accepts Conclusion-first | `queue.cpp` (`processConnectRequest`) |
| 5 | `SRTO_INITIAL_RTT`: seed RTT estimator from QUIC stats | `core.cpp`, api options |
| 6 | Verified present upstream — no patch needed: v1.5.7 ships the too-late retransmit skip as buffer state (`sndDropTooLate` in `sendmsg2` drops late data from `CSndBuffer` and purges the send loss list; a LOSSREPORT for an already-dropped seq is answered `DROPREQ`, no rexmit), equivalent-in-effect to the Rust fork's per-rexmit deadline arithmetic for a continuously-sending live publisher; receiver-side drop (`rcvDropTooLateUpTo`) completes the contract; gate-ON test added (`test_virtual_transport.cpp`) | `core.cpp` (upstream) |

Note: most of WebSRT's 12 srt-rs fork patches fixed srt-rs-specific bugs (stats never
populated, SeqNumber underflow panics, `Sub<TimeSpan>` copy-paste). The reference
libsrt does not have those; only the architectural patches above are needed.

**Milestone 0 (build first):** in-memory loopback test — two forked sockets
(skip-induction caller ↔ listener) over a virtual queue, zero syscalls; handshake,
data, retransmit, RTT seeding all green.

## Repo layout

```
websrt-cpp/
├── websrt-cpp.md        # this plan
├── PLAN.md              # (alias or merge — pick one at bootstrap)
├── AGENTS.md  README.md  LICENSE
├── CMakeLists.txt  CMakePresets.json  vcpkg.json
├── deps/                # submodules: srt (fork), lsquic
├── include/websrt/      # public API: publisher.hpp, subscriber.hpp, stats.hpp, config.hpp
├── src/
│   ├── wt/              # client.cpp, server.cpp, capsule.cpp, cert.cpp (hash pinning)
│   ├── srt_session/     # virtual_transport.cpp, subscriber.cpp, publisher.cpp
│   ├── core/            # event_loop.cpp (epoll: lsquic + srt_epoll + timers), broadcaster ring
│   ├── gateway/         # main.cpp cli.cpp gateway.cpp session.cpp registry.cpp
│   │                    # stream_registry.cpp hooks.cpp limits.cpp cert.cpp web_server.cpp
│   │                    # ingest/srt_ingest.cpp (stock-mode libsrt, UDP)
│   └── ffmpeg/          # websrt_protocol.c (URLProtocol)
├── apps/                # websrt-dump, websrt-publish-file
├── examples/ffmpeg/     # patch + build-against-upstream-ffmpeg script + golden commands
├── tests/               # loopback_srt, wt_echo, unit tests
└── scripts/             # interop.sh (drives Rust gateway + fixtures), websrt.conf for supervisord
```

C++20 for our code; submodules build with their own settings.
Toolchain: gcc 13, CMake ≥ 3.24, BoringSSL (via lsquic's LIBSSL_DIR), nasm.

## Port map (Rust → C++, gateway phase)

| Rust source | C++ equivalent |
|---|---|
| `gateway.rs` (accept loop, viewer cap, graceful drain) | `gateway.cpp` |
| `session.rs` (recv_pump, LossInjector sim-loss, QUIC stats log) | `session.cpp` |
| `registry.rs` (~2ms ticker, tick concurrency 16, credit budget 256 msgs/tick, `drop_backlog` on first connect) | `registry.cpp` |
| `stream_registry.rs` + `broadcaster.rs` (ring fanout, first-wins publish) | `stream_registry.cpp` + core ring |
| `srt_sender.rs` (SrtConfig, initiator drive, PAYLOAD_SIZE) | srt fork initiator role |
| `ingest/srt_listener.rs` (streamid required, first-wins, latency-scaled buffers 8192–320000, FC ≥ RCVBUF discipline) | `srt_ingest.cpp` (stock libsrt UDP mode) |
| `cert.rs` (self-signed regen per boot, mkcert, runtime cert-hash.js) | `cert.cpp` |
| `web_server.rs` (health endpoint, static `web/dist`) | `web_server.cpp` — serves the existing player unchanged |
| `nocc.rs` (no-op QUIC CC so SRT owns congestion control) | **no lsquic equivalent** — approximate via max pacing rate + large flow-control windows; document as behavioral difference |
| egui GUI / `gui.rs` | none — CLI-only by decision |
| `log_buffer.rs` | plain stderr/stdout logging (no GUI to feed) |

## Phases & gates

- **P0 — Bootstrap + fork seam.** Repo skeleton, submodules, CMake; `~/srt` clone +
  patches 1–6; loopback test. *Gate: M0 green.*
- **P1 — Subscribe.** wt-client + subscriber session; `websrt-dump` app.
  *Gates: dump from the live Rust gateway is byte-for-byte identical to the
  broadcaster ring; `ffprobe` clean; survives a `--sim-loss` gateway build.*
- **P2 — Publish + FFmpeg.** Publisher (listener role + sender, TS chunking
  188-aligned ≤ 1128-byte payloads); `websrt://` URLProtocol (blocking façade over
  the event-loop thread), input + output, against **upstream** FFmpeg
  (patch + build script distribution; composes with cbcrc/FFmpeg dmf-mxl for MXL,
  untested here). *Gates — the golden commands:*

  ```
  ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 -f lavfi -i sine=frequency=440 \
         -c:v libx264 -preset veryfast -c:a aac -f mpegts websrt://gw:443?stream=test
  ffmpeg -f mpegts -i websrt://gw:443?stream=test -c copy out.ts
  ```

  plus testgen → Rust gateway → browser playback.
- **P3 — Gateway.** lsquic WT server + initiator sessions + UDP ingest + fanout +
  certs + web server + supervisord conf. *Gates: unmodified browser pages play from
  the C++ gateway; Rust `mock_obs` and `wt_hs_probe` pass against it; 50-viewer
  concurrency test (ticker avg < 5 ms, zero viewer lag); parity matrix vs the Rust
  gateway.*
- **P4 — Hardening.** Config-file support, packaging, docs, CI polish.

## Testing & oracles

Interop against the existing Rust world at every phase: the live Rust gateway,
`fixtures/test.ts`, `--sim-loss` feature build, `wt_echo_client` / `mock_obs` /
`wt_hs_probe` bins, `web/smoke.mjs` WASM modules as protocol oracles, and the
browser pages as the final arbiter. CI: build + unit + loopback; interop scripts
run locally where the Rust gateway is available.

## Risks (accepted)

1. lsquic client extended-CONNECT is primitive-supported but undocumented —
   validated first thing in P1 (spike before building the full subscriber).
2. srt fork seam surgery (`CRcvQueue`/`CChannel` rewire) — bounded and config-gated;
   M0 proves it before anything builds on top.
3. No no-op QUIC congestion controller in lsquic — behavioral difference vs the
   Rust gateway's `nocc.rs`; documented, approximated via pacing/FC caps.
4. Two gateways = behavioral-parity burden — mitigated by the shared gate matrix.
5. Fork maintenance — MPL-2.0, rebase per upstream bump; patches kept minimal and
   upstreamable in spirit.

## Implementation-session kickoff checklist

1. `git init`, commit this plan, create the skeleton tree, add submodules
   (`maxolgi/srt` fork + lsquic), CMake building an empty lib.
2. Clone the fork to `~/srt` (branch `websrt` off upstream release tag `v1.5.7`,
   commit `899348d8`; fork `master` stays the clean upstream mirror); write the
   AGENTS.md fork-discipline section (mirror WebSRT's).
3. Patch 1 (virtual transport seam) + patches 3/4 (skip-induction) → M0 loopback test.
4. Proceed to P1.
