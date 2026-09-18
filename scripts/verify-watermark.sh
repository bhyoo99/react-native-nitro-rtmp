#!/usr/bin/env bash
# Checks that an overlay changed the pixels of a region: the mean per-channel difference between one frame without the
# watermark and one with it, over the watermark's rectangle, must exceed a
# threshold, while a control region far from it stays similar.
#
#   scripts/verify-watermark.sh out.flv --plain-at 2 --watermark-at 9
#                               [--region 0.7,0.05,0.25,0.1] [--threshold 40]
#
# Times are seconds into the file; the region is normalized (x,y,w,h) like
# LayerFrame. The example app's default watermark frame is the default here.
set -euo pipefail

FILE=""; PLAIN=""; MARKED=""; REGION="0.7,0.05,0.25,0.1"; THRESHOLD=40
while [ $# -gt 0 ]; do
  case "$1" in
    --plain-at) PLAIN="$2"; shift ;;
    --watermark-at) MARKED="$2"; shift ;;
    --region) REGION="$2"; shift ;;
    --threshold) THRESHOLD="$2"; shift ;;
    *) FILE="$1" ;;
  esac
  shift
done
[ -n "$FILE" ] && [ -n "$PLAIN" ] && [ -n "$MARKED" ] || { sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
command -v ffprobe >/dev/null 2>&1 || { echo "ffprobe not found on PATH" >&2; exit 2; }

SIZE="$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "$FILE")"
W="${SIZE%,*}"; H="${SIZE#*,}"
TMP="$(mktemp -d -t nitrortmp-wm)"
trap 'rm -rf "$TMP"' EXIT
ffmpeg -v error -y -ss "$PLAIN" -i "$FILE" -frames:v 1 -f rawvideo -pix_fmt rgb24 "$TMP/plain.rgb"
ffmpeg -v error -y -ss "$MARKED" -i "$FILE" -frames:v 1 -f rawvideo -pix_fmt rgb24 "$TMP/marked.rgb"

export W H REGION THRESHOLD TMP
python3 - <<'PY'
import os, sys
w, h = int(os.environ["W"]), int(os.environ["H"])
rx, ry, rw, rh = (float(v) for v in os.environ["REGION"].split(","))
threshold = float(os.environ["THRESHOLD"])
tmp = os.environ["TMP"]
plain = open(f"{tmp}/plain.rgb", "rb").read()
marked = open(f"{tmp}/marked.rgb", "rb").read()
assert len(plain) == w * h * 3 and len(marked) == w * h * 3, "frame size mismatch"

def mean_diff(x0, y0, x1, y1):
    total = 0; count = 0
    for y in range(y0, y1):
        base = (y * w + x0) * 3
        end = (y * w + x1) * 3
        a = plain[base:end]; b = marked[base:end]
        total += sum(abs(p - q) for p, q in zip(a, b))
        count += end - base
    return total / max(count, 1)

x0, y0 = int(rx * w), int(ry * h)
x1, y1 = int((rx + rw) * w), int((ry + rh) * h)
inside = mean_diff(x0, y0, x1, y1)
# Control: the same-sized region mirrored to the bottom-left corner.
cx0, cy0 = int((1 - rx - rw) * w), int((1 - ry - rh) * h)
control = mean_diff(cx0, cy0, cx0 + (x1 - x0), cy0 + (y1 - y0))
print(f"  watermark region mean |diff| = {inside:.1f} (threshold {threshold})")
print(f"  control region   mean |diff| = {control:.1f}")
ok = inside >= threshold and inside > control * 2
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
PY
