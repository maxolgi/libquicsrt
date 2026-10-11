# websrt-cpp

C++ implementation of WebSRT: `libwebsrt` (embeddable client library for
publishing and subscribing SRT-over-WebTransport streams), a CLI gateway with
Rust-gateway parity, and an FFmpeg `websrt://` protocol handler.

Companion repos: [WebSRT](https://github.com/maxolgi/WebSRT) (Rust — the
reference implementation and interop oracle) and
[maxolgi/srt](https://github.com/maxolgi/srt) (fork of Haivision/srt).

**Status: bootstrap in progress.** The design, phases, and gates live in
[websrt-cpp.md](websrt-cpp.md).

## Build

    ./build.sh setup     # one-time: submodule init, cmake configure
    ./build.sh lib       # build libwebsrt
    ./build.sh test      # run tests

Requires gcc 13+, CMake 3.24+, ninja, nasm.
