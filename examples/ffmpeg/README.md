# FFmpeg `websrt://` protocol

`websrt://host:port?stream=NAME` as input and output, against upstream
FFmpeg. Built by `./build.sh ffmpeg`: clones upstream FFmpeg at the
pinned release tag (`n7.1.5` — see `build.sh`), applies
`0001-avformat-add-websrt-protocol.patch`, and builds `deps/ffmpeg/ffmpeg`
+ `ffprobe` linked against the local `libwebsrt` (via a generated
`build/release/websrt.pc`; requires `./build.sh setup && ./build.sh lib`
with WebTransport enabled first).

## URL grammar

    websrt://host:port?stream=NAME[&cert_hash=HEX][&cert_hash_file=PATH][&pki=1][&latency_ms=N][&passphrase=…]

- **Direction decides semantics**: an *output* (URL_WRONLY) publishes
  `NAME` toward the gateway (`?publish=NAME`); an *input* subscribes
  (`?stream=`/`?subscribe=`). The same URL works verbatim on both sides.
- `stream=NAME` — required stream name (first-wins occupancy: a second
  publisher gets "stream name in use" and exit 98/`EADDRINUSE`).
- `cert_hash=HEX` — SHA-256 of the gateway cert DER (64 hex chars);
  pins like a browser's `serverCertificateHashes`.
- `cert_hash_file=PATH` — parse the gateway's runtime `cert-hash.js`
  (`window.CERT_HASH = "<hex>"` pins, `= null` selects PKI). The cert
  hash changes on every gateway restart.
- `pki=1` — normal PKI validation. **Default** when no cert parameter
  is given.
- `latency_ms=N` — TSBPD latency floor (default 120).
- `passphrase=…` — reserved, parsed and ignored (WebSRT's crypto rides
  on WebTransport's TLS; there is no WT passphrase today).

## The golden commands (P2 gate)

Canonical form (works verbatim once the gateway host/port and cert are
resolved):

    ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 -f lavfi -i sine=frequency=440 \
           -c:v libx264 -preset veryfast -c:a aac -f mpegts websrt://gw:443?stream=test
    ffmpeg -f mpegts -i websrt://gw:443?stream=test -c copy out.ts

Locally (Rust gateway under supervisord: WebTransport UDP 127.0.0.1:4433,
health 9091, self-signed cert → pin via its `cert-hash.js`):

    CERT_JS=/home/flibb/maxolgi/WebSRT/web/public/cert-hash.js
    URL="websrt://127.0.0.1:4433?stream=test&cert_hash_file=$CERT_JS"

    ./deps/ffmpeg/ffmpeg -re -f lavfi -i testsrc2=size=1280x720:rate=30 \
        -re -f lavfi -i sine=frequency=440 \
        -c:v libx264 -preset veryfast -c:a aac -t 10 -f mpegts "$URL"
    ./deps/ffmpeg/ffmpeg -f mpegts -i "$URL" -c copy -t 8 out.ts

Notes:
- `-re` paces the lavfi inputs to real time (testsrc2/sine generate as
  fast as the encoder consumes; without it a 10 s stream is pushed in a
  fraction of a second and a live subscriber cannot join it).
- `-t` goes **before** the output URL — a trailing `-t` after the last
  URL is a trailing option and gets ignored.
- `scripts/ffmpeg_gate.sh` (or `./build.sh ffmpeg-gate`) runs exactly
  this pair with a unique stream name per run, asserts exits 0, ffprobe
  cleanliness (h264 + aac, ≥ 6 s), cross-checks with `websrt-dump`, and
  runs the whole thing twice.

## Behavior details

- Writes accept any buffer size (mpegts muxer bursts); the library
  frames TS bytes into 188-aligned SRT live messages ≤ 1128 bytes.
  A write stalls longer than 30 s fails the output.
- Reads deliver one SRT message at a time through an internal bounce
  buffer, so any caller buffer size works; the protocol blocks (1 s
  poll granularity) and a 15 s dataless stall fails the input.
- A subscribing open retries the gateway's HTTP 404 for up to 10 s
  (live-edge join while a broadcast is starting); after that it fails.
- Close on the output does a graceful stream end (flush + ACK drain +
  shutdown): subscribers see EOF and the stream name frees. Stats lines
  appear at `AV_LOG_DEBUG` every 5 s and once at close (`-loglevel debug`).
