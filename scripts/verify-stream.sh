#!/usr/bin/env bash
# Checks an FLV recorded by `ffmpeg -listen` from the camera pipeline:
# codecs and size, frame rate, keyframe
# interval, timestamps, A/V alignment and a clean decode.
#
#   scripts/verify-stream.sh out.flv --width W --height H --fps F [--duration S]
#                            [--keyframe-interval 2] [--audio-rate 48000]
#   scripts/verify-stream.sh --listen [port] --width W ...   # run ffmpeg -listen
#                            (default 1935), wait for one publish, then check it
#
# Manual test loop:
#   1. scripts/verify-stream.sh --listen --width 720 --height 1280 --fps 30 --duration 10
#   2. start the example app in camera mode (EXPO_PUBLIC_SOURCE=camera) with
#      EXPO_PUBLIC_AUTOSTART_URLS="rtmp://HOST/live/test" EXPO_PUBLIC_DURATION_MS=10000
#   3. the script prints PASS or the failed checks
set -euo pipefail

usage() { sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }

FILE=""
LISTEN=0
PORT=1935
WIDTH=""
HEIGHT=""
FPS=""
DURATION=""
KEYINT=2
AUDIO_RATE=48000

while [ $# -gt 0 ]; do
  case "$1" in
    --listen)
      LISTEN=1
      if [ $# -gt 1 ] && [[ "$2" =~ ^[0-9]+$ ]]; then PORT="$2"; shift; fi
      ;;
    --width) WIDTH="$2"; shift ;;
    --height) HEIGHT="$2"; shift ;;
    --fps) FPS="$2"; shift ;;
    --duration) DURATION="$2"; shift ;;
    --keyframe-interval) KEYINT="$2"; shift ;;
    --audio-rate) AUDIO_RATE="$2"; shift ;;
    -h|--help) usage ;;
    *)
      if [ -z "$FILE" ]; then FILE="$1"; else echo "unexpected argument: $1" >&2; usage; fi
      ;;
  esac
  shift
done

[ -n "$WIDTH" ] && [ -n "$HEIGHT" ] && [ -n "$FPS" ] || { echo "--width, --height and --fps are required" >&2; usage; }
command -v ffprobe >/dev/null 2>&1 || { echo "ffprobe not found on PATH" >&2; exit 2; }
command -v ffmpeg >/dev/null 2>&1 || { echo "ffmpeg not found on PATH" >&2; exit 2; }

if [ "$LISTEN" = 1 ]; then
  FILE="${NITRORTMP_LISTEN_OUT:-$(mktemp -t nitrortmp-stream).flv}"
  echo "listening on rtmp://0.0.0.0:$PORT/live/test (any app/stream); recording to $FILE" >&2
  # -listen 1 accepts one publisher and exits when it disconnects.
  ffmpeg -hide_banner -loglevel warning -y -listen 1 -i "rtmp://0.0.0.0:$PORT/live/test" -c copy -f flv "$FILE"
fi
[ -n "$FILE" ] && [ -f "$FILE" ] || { echo "no FLV file to check" >&2; usage; }

echo "checking $FILE" >&2
STREAMS="$(ffprobe -v error -of json -show_streams "$FILE")"
VPACKETS="$(ffprobe -v error -of json -show_packets -select_streams v "$FILE")"
APACKETS="$(ffprobe -v error -of json -show_packets -select_streams a "$FILE")"
# -enc_time_base 1/1000: the null muxer would otherwise time frames in
# 1/r_frame_rate units, and a variable camera cadence (r_frame_rate guessed as
# 59/2) makes two frames share a tick, a false "non monotonically increasing dts".
DECODE_ERRORS="$(ffmpeg -v error -i "$FILE" -enc_time_base 1/1000 -f null - 2>&1 || true)"

export STREAMS VPACKETS APACKETS DECODE_ERRORS WIDTH HEIGHT FPS DURATION KEYINT AUDIO_RATE
python3 - <<'PY'
import json, os, sys

streams = json.loads(os.environ["STREAMS"])["streams"]
vpk = json.loads(os.environ["VPACKETS"])["packets"]
apk = json.loads(os.environ["APACKETS"])["packets"]
width = int(os.environ["WIDTH"]); height = int(os.environ["HEIGHT"]); fps = float(os.environ["FPS"])
duration = os.environ["DURATION"]; keyint = float(os.environ["KEYINT"]); audio_rate = int(os.environ["AUDIO_RATE"])
decode_errors = os.environ["DECODE_ERRORS"].strip()

failures = []
def check(ok, text):
    print(("  ok   " if ok else "  FAIL ") + text)
    if not ok:
        failures.append(text)

video = next((s for s in streams if s.get("codec_type") == "video"), None)
audio = next((s for s in streams if s.get("codec_type") == "audio"), None)
check(video is not None, "has a video stream")
check(audio is not None, "has an audio stream")
if video is not None:
    check(video.get("codec_name") == "h264", f"video codec h264 (got {video.get('codec_name')})")
    check(int(video.get("width", 0)) == width and int(video.get("height", 0)) == height,
          f"video size {width}x{height} (got {video.get('width')}x{video.get('height')})")
if audio is not None:
    check(audio.get("codec_name") == "aac", f"audio codec aac (got {audio.get('codec_name')})")
    check(int(audio.get("sample_rate", 0)) == audio_rate,
          f"audio sample rate {audio_rate} (got {audio.get('sample_rate')})")

def times(packets, key):
    out = []
    for p in packets:
        v = p.get(key)
        if v is None or v == "N/A":
            continue
        out.append(float(v))
    return out

vpts = times(vpk, "pts_time"); vdts = times(vpk, "dts_time")
apts = times(apk, "pts_time")
check(len(vpk) > 0, f"video packets present ({len(vpk)})")
check(len(apk) > 0, f"audio packets present ({len(apk)})")

if vpts:
    check(abs(vpts[0]) <= 0.001, f"first video pts is 0 (got {vpts[0]:.3f})")
    mono = all(b > a for a, b in zip(vpts, vpts[1:]))
    check(mono, "video pts strictly increasing")
    dmono = all(b >= a for a, b in zip(vdts, vdts[1:]))
    check(dmono, "video dts monotonic")
    span = vpts[-1] - vpts[0]
    # A camera switch stops frames for a moment while the clock
    # runs on. Such gaps are reported and excluded from the rate measurement.
    frame_gaps = [(a, b - a) for a, b in zip(vpts, vpts[1:]) if b - a > 0.250]
    gap_total = sum(g for _, g in frame_gaps)
    print(f"  info capture gaps > 250 ms: {len(frame_gaps)} (total {gap_total * 1000:.0f} ms)")
    if span - gap_total > 0:
        measured = (len(vpts) - 1 - len(frame_gaps)) / (span - gap_total)
        check(abs(measured - fps) <= fps * 0.10, f"average fps {measured:.2f} within 10% of {fps} (gaps excluded)")
    key_index = [i for i, p in enumerate(vpk) if "K" in p.get("flags", "")]
    keyframes = [vpts[i] for i in key_index]
    check(len(keyframes) >= 1 and abs(keyframes[0] - vpts[0]) <= 0.001, "first video packet is a keyframe")
    gaps = [b - a for a, b in zip(keyframes, keyframes[1:])]
    frames_between = [b - a for a, b in zip(key_index, key_index[1:])]
    tolerance = 1.0 / fps + 0.001
    # The interval is kept either in time (VideoToolbox's MaxKeyFrameIntervalDuration)
    # or in frames (MediaCodec's I-frame interval); across a capture gap the
    # frame-count rule is the one that holds.
    bad = [round(g, 3) for g, n in zip(gaps, frames_between)
           if abs(g - keyint) > tolerance and abs(n - fps * keyint) > 1]
    check(len(gaps) >= 1 or span < keyint, f"keyframes repeat ({len(keyframes)} in {span:.1f} s)")
    check(not bad, f"keyframe interval {keyint} s ±1 frame, or {fps * keyint:.0f} frames ±1 (bad gaps: {bad[:5]})")

if vpts and apts:
    check(abs(apts[0] - vpts[0]) <= 0.050, f"A/V first pts within 50 ms (audio {apts[0]:.3f}, video {vpts[0]:.3f})")
    vend = vpts[-1] + 1.0 / fps
    aend = apts[-1] + 1024.0 / audio_rate
    check(abs(vend - aend) <= 0.200, f"A/V duration within 200 ms (video {vend:.3f}, audio {aend:.3f})")
    amono = all(b > a for a, b in zip(apts, apts[1:]))
    check(amono, "audio pts strictly increasing")
    if duration:
        want = float(duration)
        check(abs(vend - want) <= max(1.0, 0.2 * want), f"video duration {vend:.2f} s close to {want} s")

check(decode_errors == "", "decodes without errors" + (f": {decode_errors[:200]}" if decode_errors else ""))

if failures:
    print(f"FAIL ({len(failures)} checks)")
    sys.exit(1)
print("PASS")
PY
