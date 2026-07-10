#!/usr/bin/env bash
# Screenshot the physical CYD over WiFi (esp32dev_uidev firmware) as a PNG.
# Usage: cyd-shot.sh <device-ip> [output.png]   (default output: .pio/sim/device.png)
set -euo pipefail

ip="${1:?usage: cyd-shot.sh <device-ip> [output.png]}"
out="${2:-.pio/sim/device.png}"

tmp="$(mktemp --suffix=.bmp)"
trap 'rm -f "$tmp"' EXIT

curl -sf --max-time 30 "http://${ip}/screenshot.bmp" -o "$tmp"
mkdir -p "$(dirname "$out")"
python3 "$(dirname "$0")/bmp2png.py" "$tmp" "$out"
echo "WROTE $out"
