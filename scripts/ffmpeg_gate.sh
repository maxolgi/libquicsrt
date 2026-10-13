#!/usr/bin/env bash
# P2 golden gate: the FFmpeg websrt:// acceptance commands, verbatim in
# form, against the live Rust gateway (supervisord service `websrt`:
# WebTransport UDP 127.0.0.1:4433, health 9091, self-signed cert whose
# hash is published in cert-hash.js).
#
#   STEP 1 (output golden): lavfi testsrc2+sine -> libx264+aac -> mpegts
#                          -> websrt://127.0.0.1:4433?stream=<UNIQUE>
#   STEP 2 (input golden, ~2 s later):
#                          mpegts <- websrt://...?stream=<SAME> -> -c copy out.ts
#
# Assertions: both steps exit 0; ffprobe -v error clean on out.ts with
# an h264 video stream and an aac audio stream; duration >= 6 s. A
# websrt-dump runs in parallel on the same stream as a cross-check
# (nonzero bytes, stats printed). The whole gate runs 2x consecutively.
#
# -re paces the lavfi inputs to real time (without it a 10 s stream is
# pushed in a fraction of a second and no live subscriber can join);
# -t sits BEFORE the output URL (a trailing -t after the last URL is
# ignored by the ffmpeg CLI).
#
# Usage: ./build.sh ffmpeg-gate  (or scripts/ffmpeg_gate.sh directly)
#   WEBSRT_DIR  Rust WebSRT checkout (default /home/flibb/maxolgi/WebSRT)

set -euo pipefail

REPO_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WEBSRT_DIR="${WEBSRT_DIR:-/home/flibb/maxolgi/WebSRT}"

FFMPEG="$REPO_DIR/deps/ffmpeg/ffmpeg"
FFPROBE="$REPO_DIR/deps/ffmpeg/ffprobe"
DUMP="$REPO_DIR/build/release/websrt-dump"
CERT_JS="$WEBSRT_DIR/web/public/cert-hash.js"
HEALTH_URL="http://127.0.0.1:9091/health"

GW_HOST=127.0.0.1
WT_PORT=4433
SUB_DELAY_S=2   # input golden starts this long after the output golden
DUMP_DELAY_S=6  # dump cross-check attaches here (streamed stagger, see below)

fail() {
    echo "ffmpeg-gate: FAIL: $*" >&2
    exit 1
}

PUB_PID=
SUB_PID=
DUMP_PID=
cleanup() {
    for pid in "$PUB_PID" "$SUB_PID" "$DUMP_PID"; do
        [ -n "$pid" ] && kill "$pid" 2>/dev/null || true
    done
    for pid in "$PUB_PID" "$SUB_PID" "$DUMP_PID"; do
        [ -n "$pid" ] && wait "$pid" 2>/dev/null || true
    done
}
trap cleanup EXIT

# ---- prerequisites ----

[ -x "$FFMPEG" ] || fail "patched ffmpeg not built (run ./build.sh ffmpeg)"
[ -x "$FFPROBE" ] || fail "patched ffprobe not built (run ./build.sh ffmpeg)"
[ -x "$DUMP" ] || fail "websrt-dump not built (run ./build.sh lib)"
[ -f "$CERT_JS" ] || fail "cert-hash.js missing: $CERT_JS (Rust gateway not running here?)"
curl -fsS -m 5 "$HEALTH_URL" >/dev/null ||
    fail "Rust gateway unhealthy at $HEALTH_URL (check: sudo supervisorctl status websrt)"

# Cert mode: parse the gateway's runtime cert-hash.js. A pinned hash
# selects `cert_hash=`; `null` (mkcert / PKI mode) selects `pki=1`.
HASH=$(sed -n 's/.*CERT_HASH *= *"\([0-9a-fA-F]\{64\}\)".*/\1/p' "$CERT_JS" | head -1)
if [ -n "$HASH" ]; then
    CERT_PARAM="cert_hash=$HASH"
elif grep -q 'CERT_HASH *= *null' "$CERT_JS"; then
    CERT_PARAM="pki=1"
    echo "ffmpeg-gate: cert mode: pki (CERT_HASH null; pinning skipped)"
else
    fail "cannot parse CERT_HASH from $CERT_JS"
fi

URL_HOST_PORT="$GW_HOST:$WT_PORT"

# ---- one gate run ----

run_gate() {
    local run="$1"
    local stream="ffmpeg-gate-$$-$(date +%s)-run$run"
    local tmp
    tmp=$(mktemp -d /tmp/websrt-ffmpeg-gate.XXXXXX)

    echo "ffmpeg-gate: run $run/$GATE_RUNS: stream=$stream"

    # (1) output golden: publish 10 s of testsrc2+sine as mpegts.
    "$FFMPEG" -hide_banner -loglevel warning \
        -re -f lavfi -i testsrc2=size=1280x720:rate=30 \
        -re -f lavfi -i sine=frequency=440 \
        -c:v libx264 -preset veryfast -c:a aac \
        -t 10 -f mpegts "websrt://$URL_HOST_PORT?stream=$stream&$CERT_PARAM" \
        >"$tmp/pub.log" 2>&1 &
    PUB_PID=$!

    # (2) input golden ~2 s in: -c copy for 8 s.
    sleep "$SUB_DELAY_S"
    "$FFMPEG" -hide_banner -loglevel warning \
        -f mpegts -i "websrt://$URL_HOST_PORT?stream=$stream&$CERT_PARAM" \
        -c copy -t 8 "$tmp/out.ts" \
        >"$tmp/sub.log" 2>&1 &
    SUB_PID=$!

    # (3) parallel cross-check: websrt-dump on the same stream, staggered
    # to +6 s. Three concurrent WT handshakes inside a few seconds
    # (pub + dump + sub) trip the local oracle's accept-loop stall
    # (documented deaf-oracle behavior); spreading them out avoids it.
    sleep $((DUMP_DELAY_S - SUB_DELAY_S))
    local dump_cert=(--pki)
    [ -n "$HASH" ] && dump_cert=(--cert-hash "$HASH")
    "$DUMP" --host "$GW_HOST" --port "$WT_PORT" --stream "$stream" \
        --out "$tmp/dump.ts" "${dump_cert[@]}" \
        --duration 8 --stats-interval 5 \
        >"$tmp/dump.log" 2>&1 &
    DUMP_PID=$!

    wait "$PUB_PID" || {
        cat "$tmp/pub.log" >&2
        PUB_PID=
        fail "run $run: output golden exited non-zero"
    }
    PUB_PID=

    wait "$SUB_PID" || {
        cat "$tmp/sub.log" >&2
        SUB_PID=
        fail "run $run: input golden exited non-zero"
    }
    SUB_PID=
    wait "$DUMP_PID" || {
        tail -5 "$tmp/dump.log" >&2
        DUMP_PID=
        fail "run $run: websrt-dump cross-check exited non-zero"
    }
    DUMP_PID=

    # (4) assertions on out.ts.
    [ -s "$tmp/out.ts" ] || fail "run $run: out.ts is empty"
    local probe
    probe=$("$FFPROBE" -v error -show_entries stream=codec_name,codec_type \
        -of csv=p=0 "$tmp/out.ts") || fail "run $run: ffprobe reported errors"
    echo "$probe" | grep -q '^h264,video$' || fail "run $run: no h264 video stream in out.ts"
    echo "$probe" | grep -q '^aac,audio$' || fail "run $run: no aac audio stream in out.ts"
    local dur
    dur=$("$FFPROBE" -v error -show_entries format=duration -of csv=p=0 "$tmp/out.ts") ||
        fail "run $run: ffprobe cannot read duration"
    awk -v d="$dur" 'BEGIN { exit !(d >= 6.0) }' ||
        fail "run $run: out.ts duration ${dur}s < 6 s"

    # (5) cross-check evidence.
    local dump_bytes
    dump_bytes=$(stat -c%s "$tmp/dump.ts")
    [ "$dump_bytes" -gt 0 ] || fail "run $run: websrt-dump received zero bytes"
    echo "ffmpeg-gate:      out.ts: duration=${dur}s $(echo "$probe" | tr '\n' ' ')"
    echo "ffmpeg-gate:      dump cross-check: $dump_bytes bytes"
    grep '^websrt-dump: done' "$tmp/dump.log" | sed 's/^/ffmpeg-gate:      /' ||
        tail -3 "$tmp/dump.log" | sed 's/^/ffmpeg-gate:      /'

    rm -rf "$tmp"
    echo "ffmpeg-gate: run $run: PASS"
}

GATE_RUNS=2
run_no=1
while [ "$run_no" -le "$GATE_RUNS" ]; do
    run_gate "$run_no"
    run_no=$((run_no + 1))
done

echo "ffmpeg-gate: PASS ($GATE_RUNS consecutive runs)"
