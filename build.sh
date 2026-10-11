#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: ./build.sh <command>

Commands:
    setup      One-time: submodule init, CMake configure (release)
    lib        Build libwebsrt (+ libsrt)
    test       Run tests via ctest (release)
    gateway    Build the C++ gateway binary
    ffmpeg     Build patched upstream FFmpeg with the websrt protocol
    interop    Run interop tests against the Rust gateway
    restart    Restart the gateway (supervisord, production only)
EOF
}

not_implemented() {
    echo "build.sh: '$1' not implemented yet (phase P1-P3)" >&2
    exit 1
}

case "${1:-}" in
    setup)
        git submodule update --init
        echo "build.sh: note: BoringSSL/lsquic setup arrives in P1"
        cmake --preset release
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
