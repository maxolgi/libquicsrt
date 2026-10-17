#!/usr/bin/env bash
# P1 interop gate: subscribe to the live Rust gateway (supervisord service
# `websrt`) with websrt-dump while a publisher streams the WebSRT fixture
# through the gateway's SRT ingest exactly once, then assert the dump is
# byte-for-byte identical to the fixture and ffprobe-clean.
#
# Local topology: WebTransport UDP 127.0.0.1:4433, SRT ingest UDP
# 127.0.0.1:9000 (streamid = stream name, no passphrase), health
# http://127.0.0.1:9091/health.
#
# Publisher: scripts/ingest_send (built on demand). Neither ffmpeg
# (re-muxes TS even with -c copy; bytes differ from the fixture) nor
# WebSRT's mock_obs / the srt-rs fork's srt-transmit (they never set a
# streamid; the ingest rejects such callers) can publish the fixture
# byte-exactly once - see scripts/ingest_send/src/main.rs.
#
# The subscriber starts BEFORE the publisher (--wait-stream): the gateway
# broadcasts only to receivers that exist when a message is sent, so a
# byte-for-byte dump must be attached before byte 0 (ingest_send waits 2 s
# after connecting for exactly that).
#
# Usage: ./build.sh interop  (or scripts/interop.sh directly)
#   WEBSRT_DIR  Rust WebSRT checkout (default $HOME/maxolgi/WebSRT)

set -euo pipefail

REPO_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WEBSRT_DIR="${WEBSRT_DIR:-$HOME/maxolgi/WebSRT}"

GW_HOST=127.0.0.1
WT_PORT=4433
SRT_PORT=9000
HEALTH_URL="http://127.0.0.1:9091/health"
DUMP_WAIT_S=30        # --wait-stream window (publisher must appear within)
DUMP_EOF_TIMEOUT_S=60 # bound for dump to reach EOF after the publisher ends

fail() {
    echo "interop: FAIL: $*" >&2
    exit 1
}

[ -x "$REPO_DIR/build/release/websrt-dump" ] || fail "websrt-dump not built (run ./build.sh lib)"
[ -f "$WEBSRT_DIR/fixtures/test.ts" ] ||
    fail "fixture missing: $WEBSRT_DIR/fixtures/test.ts (run $WEBSRT_DIR/fixtures/make-fixture.sh)"

# Cert pinning: parse the gateway's runtime cert-hash.js. `null` (mkcert /
# PKI mode) is out of scope for this gate - skip cleanly.
CERT_JS="$WEBSRT_DIR/web/public/cert-hash.js"
HASH=$(sed -n 's/.*CERT_HASH *= *"\([0-9a-fA-F]\{64\}\)".*/\1/p' "$CERT_JS" | head -1)
if [ -z "$HASH" ] && grep -q 'CERT_HASH *= *null' "$CERT_JS"; then
    echo "interop: SKIP: gateway cert is PKI/mkcert mode (CERT_HASH null); pinned-subscribe gate not applicable"
    exit 0
fi
[ -n "$HASH" ] || fail "cannot parse CERT_HASH from $CERT_JS"

# (1) gateway health - fail fast with something actionable.
curl -fsS -m 5 "$HEALTH_URL" >/dev/null ||
    fail "Rust gateway unhealthy at $HEALTH_URL (check: sudo supervisorctl status websrt)"

# (2) unique stream name per run: publish occupancy is first-wins.
STREAM="websrt-cpp-$$-$(date +%s)"
FIXTURE="$WEBSRT_DIR/fixtures/test.ts"

# (3) publisher binary (built into build/, which is gitignored).
SEND_BIN="$REPO_DIR/build/ingest_send/release/ingest_send"
if [ ! -x "$SEND_BIN" ]; then
    echo "interop: building ingest_send (one-time)..."
    CARGO_TARGET_DIR="$REPO_DIR/build/ingest_send" \
        cargo build --release --manifest-path "$REPO_DIR/scripts/ingest_send/Cargo.toml" >/dev/null
fi

TMP=$(mktemp -d /tmp/websrt-interop.XXXXXX)
DUMP_PID=
cleanup() {
    if [ -n "$DUMP_PID" ]; then
        kill "$DUMP_PID" 2>/dev/null || true
        wait "$DUMP_PID" 2>/dev/null || true
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT

# (4) subscriber first: retry Start until the publisher registers the stream.
T0=$(date +%s.%N)
"$REPO_DIR/build/release/websrt-dump" \
    --host "$GW_HOST" --port "$WT_PORT" --stream "$STREAM" \
    --out "$TMP/out.ts" --cert-hash "$HASH" \
    --wait-stream "$DUMP_WAIT_S" --stats-interval 1 \
    >"$TMP/dump.log" 2>&1 &
DUMP_PID=$!

# (5) publisher: the fixture through once, then a clean SRT close (which
# ends the stream and the viewer session behind it).
if ! "$SEND_BIN" "$GW_HOST:$SRT_PORT" "$STREAM" "$FIXTURE" >"$TMP/pub.log" 2>&1; then
    cat "$TMP/pub.log" >&2
    fail "ingest_send failed"
fi

# (6) bounded wait for the dump to reach EOF and exit on its own.
WAITED=0
while kill -0 "$DUMP_PID" 2>/dev/null; do
    if [ "$WAITED" -ge "$DUMP_EOF_TIMEOUT_S" ]; then
        tail -5 "$TMP/dump.log" >&2
        fail "websrt-dump did not reach EOF within ${DUMP_EOF_TIMEOUT_S}s"
    fi
    sleep 1
    WAITED=$((WAITED + 1))
done
if ! wait "$DUMP_PID"; then
    tail -20 "$TMP/dump.log" >&2
    fail "websrt-dump exited non-zero"
fi
DUMP_PID=

# (7) THE assertion: byte-for-byte against the fixture.
if ! cmp "$TMP/out.ts" "$FIXTURE"; then
    MSG=$(cmp "$TMP/out.ts" "$FIXTURE" 2>&1 | head -1)
    echo "interop: first difference: $MSG" >&2
    OFF=$(printf '%s\n' "$MSG" | sed -n 's/.*byte \([0-9]*\).*/\1/p')
    if [ -n "$OFF" ]; then
        START=$((OFF > 64 ? OFF - 64 : 0))
        echo "interop: dump context @ $OFF:" >&2
        od -A d -t x1z -j "$START" -N 128 "$TMP/out.ts" >&2 || true
        echo "interop: fixture context @ $OFF:" >&2
        od -A d -t x1z -j "$START" -N 128 "$FIXTURE" >&2 || true
    fi
    echo "interop: dump sizes: out=$(stat -c%s "$TMP/out.ts") fixture=$(stat -c%s "$FIXTURE")" >&2
    tail -5 "$TMP/dump.log" >&2
    fail "dump is not byte-for-byte identical to the fixture"
fi

# (8) ffprobe must be clean and see streams.
FFP_OUT=$(ffprobe -v error -show_entries format=nb_streams -of csv=p=0 "$TMP/out.ts" 2>&1) ||
    fail "ffprobe reported errors: $FFP_OUT"
[ "${FFP_OUT:-0}" -ge 1 ] || fail "ffprobe found no streams in the dump"

# (9) PASS summary.
T1=$(date +%s.%N)
DUR=$(awk -v a="$T0" -v b="$T1" 'BEGIN { printf "%.1f", b - a }')
echo "interop: PASS stream=$STREAM wall=${DUR}s"
echo "interop:      bytes fixture=$(stat -c%s "$FIXTURE") dumped=$(stat -c%s "$TMP/out.ts") (identical)"
grep '^websrt-dump: done' "$TMP/dump.log" | sed 's/^/interop:      /' || tail -3 "$TMP/dump.log"
