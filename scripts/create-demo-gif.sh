#!/usr/bin/env bash
set -euo pipefail

# Helper script to convert screencast recordings into crisp, lightweight demo GIFs for README
# Usage:
#   ./scripts/create-demo-gif.sh [input_video.webm] [output.gif] [height] [width] [x] [y] [fps]
# With no crop arguments, keeps the entire selected recording region, including attribution.
# Example:
#   ./scripts/create-demo-gif.sh ~/Videos/Screencasts/ENG.webm assets/demo.gif

INPUT_FILE="${1:-}"
OUTPUT_FILE="${2:-assets/demo.gif}"

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    sed -n '4,9p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
fi

if [ -z "$INPUT_FILE" ]; then
    # Pick the most recently modified webm file from Screencasts directory
    SCREENCAST_DIR="$HOME/Videos/Screencasts"
    if [ -d "$SCREENCAST_DIR" ]; then
        shopt -s nullglob
        for recording in "$SCREENCAST_DIR"/*.webm; do
            if [[ -z "$INPUT_FILE" || "$recording" -nt "$INPUT_FILE" ]]; then
                INPUT_FILE="$recording"
            fi
        done
    fi
fi

if [ -z "$INPUT_FILE" ] || [ ! -f "$INPUT_FILE" ]; then
    echo "Error: No input video specified and no .webm screencast found in ~/Videos/Screencasts/"
    echo "Usage: $0 [input_video.webm] [output.gif] [height] [width] [x] [y] [fps]"
    exit 1
fi

IFS=x read -r INPUT_WIDTH INPUT_HEIGHT < <(
    ffprobe -v error -select_streams v:0 -show_entries stream=width,height \
        -of csv=s=x:p=0 "$INPUT_FILE"
)
CROP_HEIGHT="${3:-$INPUT_HEIGHT}"
CROP_WIDTH="${4:-$INPUT_WIDTH}"
CROP_X="${5:-0}"
CROP_Y="${6:-0}"
FPS="${7:-30}"

for parameter in CROP_HEIGHT CROP_WIDTH CROP_X CROP_Y FPS; do
    value="${!parameter}"
    if [[ ! "$value" =~ ^[0-9]{1,6}$ ]]; then
        echo "Error: $parameter must be a non-negative integer." >&2
        exit 1
    fi
    printf -v "$parameter" '%d' "$((10#$value))"
done
if (( CROP_HEIGHT < 1 || CROP_WIDTH < 1 || FPS < 1 || FPS > 60 \
    || CROP_X + CROP_WIDTH > INPUT_WIDTH || CROP_Y + CROP_HEIGHT > INPUT_HEIGHT )); then
    echo "Error: Crop must fit ${INPUT_WIDTH}x${INPUT_HEIGHT}; FPS must be 1–60." >&2
    exit 1
fi

echo "==> Converting '$INPUT_FILE' to '$OUTPUT_FILE' (crop ${CROP_WIDTH}x${CROP_HEIGHT}+${CROP_X}+${CROP_Y})..."

mkdir -p "$(dirname "$OUTPUT_FILE")"

ffmpeg -hide_banner -loglevel warning -y -i "$INPUT_FILE" \
  -vf "crop=${CROP_WIDTH}:${CROP_HEIGHT}:${CROP_X}:${CROP_Y}:exact=1,fps=${FPS},split[s0][s1];[s0]palettegen=max_colors=128:stats_mode=diff[p];[s1][p]paletteuse=dither=bayer:bayer_scale=3" \
  -loop 0 "$OUTPUT_FILE"

SIZE=$(du -h "$OUTPUT_FILE" | cut -f1)
echo "==> Done! Saved to $OUTPUT_FILE ($SIZE)"
