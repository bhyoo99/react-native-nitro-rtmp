#!/usr/bin/env bash
# Checks an FLV recorded by `ffmpeg -listen` against the fixture reference
# (cpp/__tests__/fixtures/reference.flv), tag for tag, with the rules of the
# golden test (onMetaData, the empty AAC sequence header and the AVC
# end-of-sequence tag are ignored). flv_compare comes from the host build
# (scripts/test-cpp.sh) and is built on demand.
#
#   scripts/verify-flv.sh out.flv              # compare an existing recording
#   scripts/verify-flv.sh --listen [port]      # run ffmpeg -listen (default 1935),
#                                              # wait for one publish, then compare
#
# Manual test loop:
#   1. scripts/verify-flv.sh --listen
#   2. press Start in the example app (iOS: rtmp://127.0.0.1/live/test,
#      Android emulator: rtmp://10.0.2.2/live/test) and let one pass finish
#   3. the script prints MATCH or the first differing tag
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${NITRORTMP_BUILD_DIR:-$ROOT/build/cpp-tests}"
REFERENCE="${NITRORTMP_REFERENCE_FLV:-$ROOT/cpp/__tests__/fixtures/reference.flv}"
TOOL="$BUILD_DIR/flv_compare"

if [ $# -lt 1 ]; then
  sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//' >&2
  exit 2
fi

if [ ! -x "$TOOL" ]; then
  echo "building flv_compare ..." >&2
  NITRORTMP_BUILD_ONLY=1 NITRORTMP_TARGET=flv_compare "$ROOT/scripts/test-cpp.sh" >/dev/null
fi

if [ "$1" = "--listen" ]; then
  PORT="${2:-1935}"
  OUT="${NITRORTMP_LISTEN_OUT:-$(mktemp -t nitrortmp-listen).flv}"
  command -v ffmpeg >/dev/null 2>&1 || { echo "ffmpeg not found on PATH" >&2; exit 2; }
  echo "listening on rtmp://0.0.0.0:$PORT/live/test (any app/stream); recording to $OUT" >&2
  # -listen 1 accepts one publisher and exits when it disconnects.
  ffmpeg -hide_banner -loglevel warning -y -listen 1 -i "rtmp://0.0.0.0:$PORT/live/test" -c copy -f flv "$OUT"
  set -- "$OUT"
fi

exec "$TOOL" "$1" "$REFERENCE"
