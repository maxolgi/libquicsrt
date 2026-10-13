# libwebsrt — outstanding items

P0–P2 shipped and pushed (master on github.com/maxolgi/libwebsrt). The
original plan doc was purged after completion — the Rust→C++ gateway port
map for the gateway repo lives in `libwebsrt.md`'s git history (last
commit touching it, before removal).

## Human actions

- [ ] Create `websrt-gateway-cpp` on GitHub, then mine the gateway plan
      (P3/P4 phases, port map) from this repo's git history —
      `git log --follow -- libwebsrt.md`.
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

## Observations for the WebSRT repo (Rust, separate repo — fix there)

- **Player video decode needs software decode on this box**: with the debug
  panel's Codec tab set to "Prefer Hardware" (VAAPI/NVIDIA present but
  producing choppy output), `VideoDecoder.configure()` throws
  `OperationError: Unsupported configuration` and the canvas stays black.
  Switching to "Prefer Software" in the Codec tab decodes and renders
  perfectly (verified: 237 frames, 0 dropped, 30 FPS). Not a gateway issue —
  identical on the Rust gateway. Worth remembering when a stream "won't
  play" on a machine with VAAPI.
- Gateway accept-loop stall: ≥3 concurrent WT handshakes within seconds can
  stall the wtransport accept loop 10–20 s (`BrowserSession::create().await`
  / registry insert timing suspected). Sessions attaching during recovery
  can receive `messages_pushed=0` until EOF. Short client connect timeouts
  worsen it (abandoned handshakes pile up as `TimedOut`). The
  websrt-gateway-cpp gateway must not reproduce this.
- 404-reset race: the gateway sometimes resets the CONNECT stream before
  the client reads the 404 — client sees a transport failure instead of a
  clean rejection. Flush the response before resetting.
