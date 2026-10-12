# websrt-cpp — outstanding items

Status after the P0–P2 sessions (fork `websrt` @ e3de15a pushed; repo master
at 61a0ef4, unpushed). Gates green: M0 loopback, P1 byte-for-byte interop +
ffprobe, P2 golden commands 2×.

## Human actions

- [ ] Review and push websrt-cpp master (22 commits, `45b6e75`…`61f0ef4`).
- [ ] Browser arbiter (P2 final check): publish via the local golden command
      (see `examples/ffmpeg/README.md`), then open the WebSRT player against
      the local gateway — needs the WebSRT web dev server (`./build.sh web`
      in ~/maxolgi/WebSRT; the local supervisord `websrt` runs without web
      UI). Stream should play in the browser.
- [ ] Decide on the sim-loss rebuild of the Rust gateway (below) — it swaps
      the locally running supervisord service to a `--sim-loss` feature build.

## Deferred — this repo

- P1 residual gate item: survive a `--sim-loss` gateway build. Requires
  rebuilding the Rust gateway with the feature (`./build.sh gateway
  --sim-loss` in ~/maxolgi/WebSRT), restarting the local service, re-running
  `scripts/interop.sh`, then restoring the normal build. Not yet done.
- P2 tuning: `Publisher::End(5 s)` frequently reports "send buffer did not
  drain in time" (WT pacing vs TSBPD at stream tail); falls back to `Stop()`
  cleanly and the name frees, but the drain window deserves tuning in P3.
- P3 — the C++ gateway (next major work):
  1. lsquic WT server accept + cert modes (self-signed regen-per-boot,
     mkcert PEM, runtime `cert-hash.js` emission)
  2. Per-session SRT initiator (caller, skip-induction, cookie 0) + ~2 ms
     ticker (concurrency 16, credit budget 256 msgs/tick, `drop_backlog` on
     first connected tick)
  3. Broadcaster ring + stream registry (first-wins occupancy, TS bytes
     never modified)
  4. SRT/UDP ingest via stock-mode libsrt (streamid routing, latency-scaled
     buffers 8192–320000, FC ≥ RCVBUF)
  5. Web server: static `web/dist` serving + health endpoint; supervisord
     conf (`scripts/websrt-cpp.conf`)
  6. No-op-CC approximation: max pacing rate + large FC windows (documented
     behavioral difference vs Rust `nocc.rs`)
  Gates: unmodified browser pages play from the C++ gateway; Rust
  `wt_hs_probe` / `mock_obs` pass against it; 50-viewer concurrency
  (ticker avg < 5 ms, zero viewer lag); parity matrix vs the Rust gateway.
- P4 — hardening: config-file support, packaging, docs, CI polish.

## Observations for the WebSRT repo (Rust, separate repo — fix there)

- Gateway accept-loop stall: ≥3 concurrent WT handshakes within seconds can
  stall the wtransport accept loop 10–20 s (`BrowserSession::create().await`
  / registry insert timing suspected). Sessions attaching during recovery
  can receive `messages_pushed=0` until EOF. Short client connect timeouts
  worsen it (abandoned handshakes pile up as `TimedOut`). Our P3 gateway
  must not reproduce this.
- 404-reset race: the gateway sometimes resets the CONNECT stream before
  the client reads the 404 — client sees a transport failure instead of a
  clean rejection. Flush the response before resetting.

## Known behavioral quirks (documented, intentional)

- Negotiated payload lands at 940 bytes (Rust puts 1128 in the HSv5 MTU
  field; fork's `min()` then computes MSS 1128 → payload 1084 → chunker
  floors 940 via `SRTO_MSS` readback). Byte-exactness unaffected.
- `bytes_received`/`bytes_sent` stats include 44 B/packet SRT header
  overhead; Read/Write payloads are pure TS bytes.
- ffmpeg URL: direction decides semantics — output publishes, input
  subscribes; same URL verbatim both ways.
