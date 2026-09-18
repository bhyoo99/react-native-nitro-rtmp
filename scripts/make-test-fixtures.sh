#!/usr/bin/env bash
# Regenerates cpp/__tests__/fixtures with ffmpeg. Run only when the fixture
# parameters change; the golden test compares against reference.flv byte for byte.
#
#   video.h264     320x240, 30 fps, 2 s, H.264 Constrained Baseline (CAVLC), no
#                  B-frames, one slice per frame, keyframe every 30 frames with
#                  SPS/PPS repeated in front of every IDR (x264 repeat-headers).
#   audio.aac      AAC-LC 48 kHz stereo ADTS (protection_absent=1), 2 s of a 440 Hz sine.
#   reference.flv  ffmpeg -c copy of both into FLV with -fflags +bitexact. Video/audio
#                  tag bodies and timestamps are the expected publisher output.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/cpp/__tests__/fixtures"
FFMPEG="${FFMPEG:-ffmpeg}"

command -v "$FFMPEG" >/dev/null || { echo "ffmpeg not found (set FFMPEG=/path/to/ffmpeg)" >&2; exit 1; }
mkdir -p "$OUT"

COMMON=(-hide_banner -loglevel error -y -fflags +bitexact -flags +bitexact)

"$FFMPEG" "${COMMON[@]}" \
  -f lavfi -i "testsrc2=size=320x240:rate=30" -t 2 \
  -c:v libx264 -preset ultrafast -profile:v baseline -pix_fmt yuv420p \
  -bf 0 -g 30 -keyint_min 30 -sc_threshold 0 -threads 1 \
  -x264-params "repeat-headers=1:sliced-threads=0:annexb=1" \
  -b:v 250k -maxrate 250k -bufsize 500k \
  -f h264 "$OUT/video.h264"

"$FFMPEG" "${COMMON[@]}" \
  -f lavfi -i "sine=frequency=440:sample_rate=48000" -t 2 \
  -ac 2 -c:a aac -b:a 96k \
  -f adts "$OUT/audio.aac"

"$FFMPEG" "${COMMON[@]}" \
  -framerate 30 -i "$OUT/video.h264" \
  -i "$OUT/audio.aac" \
  -c copy -f flv "$OUT/reference.flv"

ls -l "$OUT"
