# libwebsrt

C++ implementation of WebSRT, scoped to the client library and FFmpeg
integration: `libwebsrt` (embeddable library for publishing and subscribing
SRT-over-WebTransport streams) and an FFmpeg `websrt://` protocol handler.
The C++ gateway lives in the separate `websrt-gateway-cpp` repo.

Companion repos: [WebSRT](https://github.com/maxolgi/WebSRT) (Rust — the
reference implementation and interop oracle) and
[maxolgi/srt](https://github.com/maxolgi/srt) (fork of Haivision/srt).

**Status: P0–P2 shipped** — the client library and FFmpeg protocol are
gate-green against the Rust gateway (byte-exact interop, golden commands).

## Build

    ./build.sh setup     # one-time: submodule init, cmake configure
    ./build.sh lib       # build libwebsrt
    ./build.sh test      # run tests

Requires gcc 13+, CMake 3.24+, ninja, nasm.
