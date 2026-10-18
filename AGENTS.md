# libwebsrt

C++ client library for WebSRT (SRT live streaming over HTTP/3
WebTransport): Publisher/Subscriber façades with a blocking,
exception-free API, an extern-C ABI (`include/websrt/websrt_c.h`) for
FFmpeg's `websrt://` protocol, talking to any WebSRT gateway.

## Build & test

    ./build.sh setup     # one-time: submodule init + BoringSSL fetch/build + cmake configure
    ./build.sh lib       # build everything (ninja, build/release)
    ./build.sh test      # ctest --preset release; no external services needed

- Order matters: `setup` once, then `lib`/`test` freely.
- `WEBSRT_ENABLE_WT=OFF ./build.sh setup` = srt-only fast path (no
  BoringSSL, no lsquic, no WebTransport code) for quick iteration.
- Single test: `ctest --preset release -R capsule_test`.
- Sanitizers: `cmake --preset asan && cmake --build --preset asan &&
  ctest --preset asan` (likewise `tsan`; suppression file
  `cmake/tsan.supp`). CI runs release + asan + tsan.
- Toolchain: gcc 13+, CMake 3.24+, ninja, nasm and go (BoringSSL),
  zlib1g-dev.

## Live gateway (supervisord)

Live testing uses the supervised `websrt-cpp` gateway (test stream
`browsertest` from `websrt-cpp-testpub`). WT 127.0.0.1:4435, health
http://127.0.0.1:9292, config `/etc/websrt-cpp/gateway.conf`, logs
`/var/log/websrt-cpp/`. Cert pin: `curl -k
https://127.0.0.1:8443/cert-hash.js` (changes every boot). Restart:
`sudo supervisorctl restart websrt-cpp`. ctest is fully offline.

## Dependencies (deps/)

- `deps/srt` — submodule: the fork `maxolgi/srt`, branch `websrt`
  (virtual transport instead of OS sockets, no built-in crypto). Bumps
  are normal commits: "Bump deps/srt to <sha>: ...".
- `deps/lsquic` — submodule (fetches ls-qpack/ls-hpack recursively).
- `deps/boringssl` and `deps/ffmpeg` — fetched at pinned commits by
  `build.sh`, gitignored. Never commit anything under them, and never
  commit `cert-hash.js` (runtime artifact).

## CMake flavors and the srt symbol rule

- `WEBSRT_ENABLE_WT` (default ON) — WebTransport via lsquic+BoringSSL.
- `WEBSRT_BUILD_SHARED` — `libwebsrt.so` exporting only the `websrt_*`
  C ABI (`cmake/websrt_shared.map`); the flavor FFmpeg links.
- `WEBSRT_RECEIVER_ONLY` — WT-free `websrt::receiver-core` for the
  Emscripten consumer (websrt-wasm-cpp).
- **Never link the static chain (`-lwebsrt -lsrt -llsquic -lssl
  -lcrypto`) into anything that also links a system srt.** One flat
  `srt_*` symbol namespace per process: whichever copy the linker binds
  first silently wins and breaks the other. That collision is what the
  shared flavor exists to avoid (README "Coexisting with system srt").
- Out-of-tree consumers link only the exported aliases `websrt::websrt`,
  `websrt::srt-deps`, `websrt::wt-deps`. `tests/consumer/` compiles
  against exactly those with no source-tree include paths — keep it
  green when touching exports.

## Layout

- `include/websrt/` — public API. `src/core/` (epoll EventLoop),
  `src/wt/` (lsquic WebTransport client), `src/srt_session/` (forked-srt
  session layers, TsChunker), `src/websrt/` (façades).
- `apps/websrt-dump.cpp` — a complete subscriber example.
- `examples/ffmpeg/` — the committed FFmpeg protocol patch (cut against
  the pinned master snapshot; `deps/ffmpeg` is a fetched scratch copy,
  not the source of truth).

## Style

- `.clang-format` (LLVM base, 4-space, Allman, 120 cols, unsorted
  includes), `.clang-tidy` curated checks. C++20, `CXX_EXTENSIONS OFF`
  everywhere.
- Tests: no framework, no asserts — Release-safe `Check()` lambdas and
  typed status expectations; follow the existing `tests/*.cpp` pattern.
- Commits: lowercase `area: summary` (see `git log`).
