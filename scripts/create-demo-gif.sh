#!/usr/bin/env bash
set -euo pipefail

# Helper script to convert screencast recordings into crisp, lightweight demo GIFs for README
# Usage:
#   ./scripts/create-demo-gif.sh [input_video.webm] [output.gif] [crop_height]
# Example:
#   ./scripts/create-demo-gif.sh ~/Videos/Screencasts/ENG.webm assets/demo.gif 30

INPUT_FILE="${1:-}"
OUTPUT_FILE="${2:-assets/demo.gif}"
CROP_HEIGHT="${3:-30}"

if [ -z "$INPUT_FILE" ]; then
    # Pick the most recently modified webm file from Screencasts directory
    SCREENCAST_DIR="$HOME/Videos/Screencasts"
    if [ -d "$SCREENCAST_DIR" ]; then
        INPUT_FILE="$(ls -t "$SCREENCAST_DIR"/*.webm 2>/dev/null | head -n 1 || true)"
    fi
fi

if [ -z "$INPUT_FILE" ] || [ ! -f "$INPUT_FILE" ]; then
    echo "Error: No input video specified and no .webm screencast found in ~/Videos/Screencasts/"
    echo "Usage: $0 [input_video.webm] [output.gif] [crop_height]"
    exit 1
fi

echo "==> Converting '$INPUT_FILE' to '$OUTPUT_FILE' (crop 600x${CROP_HEIGHT})..."

mkdir -p "$(dirname "$OUTPUT_FILE")"

ffmpeg -y -i "$INPUT_FILE" \
  -vf "crop=600:${CROP_HEIGHT}:0:0,fps=30,split[s0][s1];[s0]palettegen=max_colors=128:stats_mode=diff[p];[s1][p]paletteuse=dither=bayer:bayer_scale=3" \
  "$OUTPUT_FILE"

SIZE=$(du -h "$OUTPUT_FILE" | cut -f1)
echo "==> Done! Saved to $OUTPUT_FILE ($SIZE)"
