#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: ./build.sh <command>

Commands:
    setup      One-time: submodule init, BoringSSL fetch+build, CMake configure (release)
    lib        Build libwebsrt (+ libsrt, lsquic)
    test       Run tests via ctest (release)
    gateway    Build the C++ gateway binary
    ffmpeg     Build patched upstream FFmpeg with the websrt protocol
    interop    Run interop tests against the Rust gateway
    restart    Restart the gateway (supervisord, production only)

Environment:
    WEBSRT_ENABLE_WT=OFF  setup without WebTransport: skips BoringSSL and
                          configures CMake with -DWEBSRT_ENABLE_WT=OFF
                          (M0/CI fast path, no lsquic either)
EOF
}

not_implemented() {
    echo "build.sh: '$1' not implemented yet (phase P1-P3)" >&2
    exit 1
}

# Pinned per lsquic's README.md and appveyor-linux.yml (tag 0.20250807.0 is
# the revision lsquic CI builds against).
BORINGSSL_COMMIT=e20e8486554098772673b55ea76bb003d0b330e5

# deps/boringssl is fetched (not a submodule, gitignored) and built
# out-of-source; idempotent: no-op clone when the pin matches, fetch+checkout
# otherwise, incremental rebuild when already built.
setup_boringssl() {
    if [ ! -d deps/boringssl/.git ]; then
        git clone https://github.com/google/boringssl.git deps/boringssl
    fi
    if [ "$(git -C deps/boringssl rev-parse HEAD)" != "$BORINGSSL_COMMIT" ]; then
        git -C deps/boringssl fetch origin
        git -C deps/boringssl checkout -f "$BORINGSSL_COMMIT"
    fi
    cmake -S deps/boringssl -B deps/boringssl/build \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
    cmake --build deps/boringssl/build -j "$(nproc)"
}

case "${1:-}" in
    setup)
        # lsquic declares the ls-qpack and ls-hpack submodules, so --recursive
        git submodule update --init --recursive
        wt="${WEBSRT_ENABLE_WT:-ON}"
        if [ "$wt" = "ON" ]; then
            setup_boringssl
        fi
        cmake --preset release -DWEBSRT_ENABLE_WT="$wt"
        ;;
    lib)
        cmake --build --preset release
        ;;
    test)
        ctest --preset release
        ;;
    gateway | ffmpeg | interop | restart)
        not_implemented "$1"
        ;;
    -h | --help | help)
        usage
        ;;
    *)
        usage >&2
        exit 1
        ;;
esac
