# libwebsrt — outstanding items

Status after the P0–P2 sessions (fork `websrt` @ e3de15a pushed; repo master
at 61a0ef4, unpushed). Gates green: M0 loopback, P1 byte-for-byte interop +
ffprobe, P2 golden commands 2×.

## Human actions

- [ ] Rename the GitHub repo (new home `maxolgi/libwebsrt`; the local origin
      already points there — nothing pushed yet).
- [ ] Create `websrt-gateway-cpp` on GitHub (the gateway phases P3/P4 move
      there; a follow-up session mines the gateway plan text from this repo's
      git history).
- [ ] Push both repos.

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
  cleanly and the name frees, but the drain window deserves a later tuning
  pass.
- P3/P4 — the gateway phases (lsquic WT server, SRT initiator, broadcaster
  fanout, UDP ingest, cert modes, web server, then hardening) live in the
  `websrt-gateway-cpp` repo now; the original breakdown is preserved in this
  file's git history for the gateway repo to mine.

## Observations for the WebSRT repo (Rust, separate repo — fix there)

- **Player video decode broken on Chrome 151**: `VideoDecoder.configure()`
  in the player throws `OperationError: Unsupported configuration`; the canvas
  stays black (audio decodes, SRT stats clean). Identical against the Rust
  gateway and the C++ gateway — player-side, not a gateway defect. H.264
  `isConfigSupported()` is true in that browser, so the configure call needs
  review (`web/src/decode.ts`).
- Gateway accept-loop stall: ≥3 concurrent WT handshakes within seconds can
  stall the wtransport accept loop 10–20 s (`BrowserSession::create().await`
  / registry insert timing suspected). Sessions attaching during recovery
  can receive `messages_pushed=0` until EOF. Short client connect timeouts
  worsen it (abandoned handshakes pile up as `TimedOut`). The
  websrt-gateway-cpp gateway must not reproduce this.
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
