#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: ./build.sh <command>

Commands:
    setup      One-time: submodule init, BoringSSL fetch+build, CMake configure (release)
    lib        Build libwebsrt (+ libsrt, lsquic)
    test       Run tests via ctest (release)
    install    Stage a cmake --install tree ([prefix], default build/release/stage)
    ffmpeg     Build patched upstream FFmpeg with the websrt protocol
    ffmpeg-gate Run the P2 golden-commands gate (needs the Rust gateway + ffmpeg)
    interop    Run interop tests against the Rust gateway

Environment:
    WEBSRT_ENABLE_WT=OFF  setup without WebTransport: skips BoringSSL and
                          configures CMake with -DWEBSRT_ENABLE_WT=OFF
                          (M0/CI fast path, no lsquic either)
EOF
}

# Pinned per lsquic's README.md and appveyor-linux.yml (tag 0.20250807.0 is
# the revision lsquic CI builds against).
BORINGSSL_COMMIT=e20e8486554098772673b55ea76bb003d0b330e5

# Pinned FFmpeg master snapshot: the websrt protocol patch
# (examples/ffmpeg/) is cut against master for upstream submission, so the
# local gate builds that same snapshot. Deliberately pinned for
# reproducibility; bumping = update the SHA, re-verify ./build.sh ffmpeg
# + the golden gate. deps/ffmpeg is fetched (not a submodule, gitignored)
# and the committed patch is applied on top.
FFMPEG_MASTER_COMMIT=24e860f6d3c4f2ebae65ed7649e2a8860d512879

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

# Stage a cmake --install tree: libwebsrt.a + static dep chain + headers +
# websrt.pc under the given prefix (default build/release/stage). The
# ffmpeg target points pkg-config at <stage>/lib/pkgconfig.
stage_install() {
    local prefix="${1:-$PWD/build/release/stage}"
    cmake --install build/release --prefix "$prefix"
}

# Patched upstream FFmpeg with the websrt:// protocol (examples/ffmpeg/).
# deps/ffmpeg is fetched (gitignored) at the pinned master snapshot, the
# committed patch applied, and ffmpeg + ffprobe built against libwebsrt
# via the staged install prefix's websrt.pc (cmake --install). Idempotent:
# skips the clone when the pin matches, the apply when the tree is already
# patched, and the compile is incremental.
build_ffmpeg() {
    local patch="examples/ffmpeg/0001-avformat-add-websrt-protocol.patch"
    local repo_dir="$PWD"

    [ -f build/release/libwebsrt.a ] || {
        echo "build.sh: ffmpeg: libwebsrt not built (run ./build.sh setup && ./build.sh lib)" >&2
        exit 1
    }
    [ -f deps/boringssl/build/libssl.a ] || {
        echo "build.sh: ffmpeg: BoringSSL not built (run ./build.sh setup with WEBSRT_ENABLE_WT=ON)" >&2
        exit 1
    }

    stage_install
    [ -f "$repo_dir/build/release/stage/lib/pkgconfig/websrt.pc" ] || {
        echo "build.sh: ffmpeg: staged websrt.pc missing (run ./build.sh install)" >&2
        exit 1
    }

    # A raw SHA can't be used with `git clone --branch`, so init + fetch
    # the pin directly. reset+clean matter: an already-patched tree would
    # otherwise leave the patch's untracked files (websrtproto.c) behind,
    # breaking the apply step below.
    if [ ! -d deps/ffmpeg/.git ]; then
        git init -q deps/ffmpeg
        git -C deps/ffmpeg remote add origin https://github.com/FFmpeg/ffmpeg.git
    fi
    if [ "$(git -C deps/ffmpeg rev-parse HEAD)" != "$FFMPEG_MASTER_COMMIT" ]; then
        git -C deps/ffmpeg fetch --depth 1 origin "$FFMPEG_MASTER_COMMIT"
        git -C deps/ffmpeg checkout -f --detach "$FFMPEG_MASTER_COMMIT"
        git -C deps/ffmpeg reset --hard HEAD
        git -C deps/ffmpeg clean -fdq
    fi

    if [ ! -f deps/ffmpeg/libavformat/websrtproto.c ]; then
        git -C deps/ffmpeg apply "$repo_dir/$patch"
    fi

    # libx264 for the golden output command (install when missing).
    if ! pkg-config --exists x264; then
        echo "build.sh: ffmpeg: installing libx264-dev..."
        sudo apt-get install -y libx264-dev || {
            echo "build.sh: ffmpeg: libx264-dev unavailable; the golden gate cannot run" >&2
            exit 1
        }
    fi

    (cd deps/ffmpeg && PKG_CONFIG_PATH="$repo_dir/build/release/stage/lib/pkgconfig" ./configure \
        --enable-gpl --enable-libx264 --enable-libwebsrt)
    make -C deps/ffmpeg -j "$(nproc)" ffmpeg ffprobe
    echo "build.sh: ffmpeg: binaries at deps/ffmpeg/ffmpeg and deps/ffmpeg/ffprobe"
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
    install)
        stage_install "${2:-}"
        ;;
    ffmpeg)
        build_ffmpeg
        ;;
    ffmpeg-gate)
        exec scripts/ffmpeg_gate.sh
        ;;
    interop)
        exec scripts/interop.sh
        ;;
    -h | --help | help)
        usage
        ;;
    *)
        usage >&2
        exit 1
        ;;
esac
