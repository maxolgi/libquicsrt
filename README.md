# libquicsrt

**libquicsrt** is a C++ client library for QuicSRT — SRT live streaming over
WebTransport. It publishes and subscribes MPEG-TS streams to any QuicSRT
gateway, from plain C++ or directly from FFmpeg through the `quicsrt://`
protocol, and interops with the reference
[QuicSRT](https://github.com/maxolgi/QuicSRT) stack: the Rust gateway, its
UDP ingest, and the browser player.

QuicSRT carries SRT's live-streaming semantics — TSBPD latency control,
retransmission, too-late drops — over HTTP/3 WebTransport, so the same
stream reaches native clients and browsers alike. WebTransport's TLS
replaces the SRT handshake's UDP cookie exchange: sessions connect with a
single 1-RTT handshake, and self-signed gateways are pinned the way
browsers pin them (SHA-256 of the certificate DER).

## Features

- Publisher and Subscriber with a blocking, exception-free C++ API and an
  extern-C ABI (`quicsrt/quicsrt_c.h`) for FFmpeg and other C consumers
- `quicsrt://` FFmpeg protocol — the same URL publishes as an output and
  subscribes as an input, with live-edge join retries and bounded stalls
- MPEG-TS framing: 188-aligned SRT live messages of at most 1128 bytes
  (6 TS packets), any caller chunk size, negotiated payload floor
- Certificate pinning or normal PKI validation; hostname verification
- SRT statistics snapshots (sender and receiver views)
- Typed session outcomes: rejected (name held / unknown stream), connect
  timeout, transport failure — each distinguishable, none throwing
- Single epoll event loop per session; cross-thread Stop with bounded
  wake-up; graceful stream end that gives subscribers a clean EOF

## Usage

With a [patched upstream FFmpeg](examples/ffmpeg) (`./build.sh ffmpeg`):

    ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 -f lavfi -i sine=frequency=440 \
           -c:v libx264 -preset veryfast -c:a aac -f mpegts quicsrt://gw:443?stream=test
    ffmpeg -f mpegts -i quicsrt://gw:443?stream=test -c copy out.ts

The same URL publishes and subscribes; `?stream=` names the stream. Pin a
self-signed gateway with `?cert_hash=<64 hex>` or
`?cert_hash_file=cert-hash.js` (the gateway's runtime file); omit the
parameter for PKI.

From C++:

```cpp
quicsrt::PublisherConfig cfg;
cfg.host = "gw.example.net";
cfg.stream_name = "test";
cfg.cert_sha256_hex = "<64 hex chars from the gateway's cert-hash.js>";

quicsrt::Publisher pub;
if (pub.Start(cfg) == quicsrt::kPublishOk) {
    pub.Write(ts_chunk, ts_len, 1000);   // any chunk size; returns bytes accepted
    pub.End(5000);                       // graceful end: subscribers see EOF
}
```

See `include/quicsrt/` for the full API (`Publisher`, `Subscriber`, configs,
stats) and `apps/quicsrt-dump.cpp` for a complete subscriber example.

## Building

    ./build.sh setup     # one-time: submodule init/update (srt fork, lsquic), BoringSSL, cmake configure
    ./build.sh lib       # build libquicsrt (+ apps)
    ./build.sh test      # unit + loopback tests
    ./build.sh ffmpeg    # patched FFmpeg with quicsrt:// AND system srt:// (see below)

Requires gcc 13+, CMake 3.24+, ninja, nasm. The srt fork
([maxolgi/srt](https://github.com/maxolgi/srt), branch `quicsrt`) and lsquic
build as submodules; BoringSSL is fetched by the setup step.

## Coexisting with system srt

FFmpeg's `srt://` protocol links a normal libsrt (Debian:
`libsrt-openssl-dev`, auto-installed by `./build.sh ffmpeg`). libquicsrt
embeds a *fork* of srt — virtual transport, no built-in crypto — and ELF
has one flat `srt_*` symbol namespace per process, so two srt copies in
one binary fight over it: whichever the linker binds first silently wins,
breaking the other (passphrase-less fork code serving `srt://`, or
`quicsrt://` losing its fork-only API). A default-visibility shared build
does not fix that; it only makes the interposition nondeterministic.

The fix is namespace isolation. `./build.sh ffmpeg` stages the **shared
flavor** of libquicsrt — `QUICSRT_BUILD_SHARED=ON`, a `libquicsrt.so` with
the srt fork, lsquic, and BoringSSL linked in and localized by a linker
version script (`cmake/quicsrt_shared.map`). The DSO exports exactly the
extern-C ABI (`quicsrt_*` from `quicsrt/quicsrt_c.h`) and nothing else, so
one FFmpeg binary carries both protocols with zero shared symbols:

    ffmpeg -f mpegts -i quicsrt://gw:4433?stream=cam1 -c copy \
           -f mpegts srt://obs:9000?streamid=cam1     # both stacks, one process
```

The static flavor stays the default and unchanged — in-tree consumers
(the C++ gateway, the internal tests) call the C++ API and the fork's
sockets directly and need those symbols visible. Do not link the static
chain (`-lquicsrt -lsrt -llsquic -lssl -lcrypto`) into anything that also
links a system srt; that is the collision the shared flavor exists to
avoid.

## License

[MPL-2.0](LICENSE).
