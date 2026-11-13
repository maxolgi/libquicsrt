# FFmpeg `quicsrt://` protocol

`quicsrt://host:port?stream=NAME` as input and output, against upstream
FFmpeg. Built by `./build.sh ffmpeg`: fetches upstream FFmpeg at the
pinned master snapshot (`FFMPEG_MASTER_COMMIT` in `build.sh`),
applies `0001-avformat-add-libquicsrt-protocol.patch`, and builds
`deps/ffmpeg/ffmpeg` + `ffprobe` linked against the staged shared
`libquicsrt.so` flavor (pkg-config prefix `build/shared/stage`, staged
by the target itself). Full chain (WebTransport enabled):

    ./build.sh setup && ./build.sh ffmpeg

Bumping the FFmpeg pin = update `FFMPEG_MASTER_COMMIT` and re-verify
`./build.sh ffmpeg`.

## URL grammar

    quicsrt://host:port?stream=NAME[&cert_hash=HEX][&pki=0|1][&latency_ms=N]

- **Direction decides semantics**: an *output* (URL_WRONLY) publishes
  `NAME` toward the gateway (`?publish=NAME`); an *input* subscribes
  (`?stream=`/`?subscribe=`). The same URL works verbatim on both sides.
- `stream=NAME` — required stream name (first-wins occupancy: a second
  publisher gets "stream name in use" and exit 98/`EADDRINUSE`).
- `cert_hash=HEX` — SHA-256 of the gateway cert DER (64 hex chars);
  pins like a browser's `serverCertificateHashes`.
- `pki=0|1` — normal PKI validation (`1`). **Default** when no cert
  parameter is given; mutually exclusive with `cert_hash`.
- `latency_ms=N` — TSBPD latency floor (default 120).
- All parameters are also protocol AVOptions (e.g. `ffmpeg -stream NAME
  -i quicsrt://gw:4435`); a URL parameter takes precedence over the
  option value. String values are percent-decoded
  (`stream=my%20stream`); `pki` accepts `0` or `1`.

## The golden commands

Canonical form (works verbatim once the gateway host/port and cert are
resolved):

    ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 -f lavfi -i sine=frequency=440 \
           -c:v libx264 -preset veryfast -c:a aac -f mpegts quicsrt://gw:443?stream=test
    ffmpeg -f mpegts -i quicsrt://gw:443?stream=test -c copy out.ts

Locally, run the pair against the supervised `QuicSRT-gateway`
(ports per its live config, `/etc/QuicSRT-gateway/gateway.conf` — currently
WT 4435, web 8443; never launch an ad-hoc gateway). The gateway's cert
is self-signed, so pin it: fetch `cert-hash.js` and pull the 64-hex
hash out in the shell:

    CERT_HASH=$(curl -fsS -k https://127.0.0.1:8443/cert-hash.js \
        | sed -n 's/.*"\([0-9a-fA-F]\{64\}\)".*/\1/p')
    test -n "$CERT_HASH" || { echo "no cert hash in cert-hash.js" >&2; exit 1; }
    URL="quicsrt://127.0.0.1:4435?stream=test&cert_hash=$CERT_HASH"

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

## Behavior details

- Writes accept any buffer size (mpegts muxer bursts); the library
  frames TS bytes into 188-aligned SRT live messages ≤ 1128 bytes.
  A write stalled longer than 30 s fails the output.
- Reads deliver one SRT message at a time through an internal bounce
  buffer, so any caller buffer size works; the protocol blocks (1 s
  poll granularity) and a 15 s dataless stall fails the input.
- The generic `rw_timeout` (ffmpeg `-timeout`, microseconds) overrides
  both stall bounds; the AVIO interrupt callback is honored in the
  read/write loops and short-circuits graceful teardown at close, so
  Ctrl+C stays responsive during stalls.
- Opens are a single attempt: connect-level failures fail fast (the
  library's connect timeout, 10 s default), a gateway 404 fails the
  subscribe immediately (`ECONNREFUSED`), and a gateway that holds a
  publisher-less session open leaves the input to the 15 s read stall
  bound. Start the publisher before the subscriber.
- Close on the output does a graceful stream end (flush + ACK drain +
  shutdown): subscribers see EOF and the stream name frees. Stats lines
  appear at `AV_LOG_DEBUG` every 5 s and once at close (`-loglevel debug`).
