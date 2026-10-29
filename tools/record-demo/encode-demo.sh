#!/usr/bin/env bash
# lambdock -- hyper-hackable desktop dock for Wayland
#
# SPDX-License-Identifier: MIT
# Copyright © Josep Bigorra <jjbigorra@gmail.com>

#
# Turn a raw recording plus its event log into the showcase GIF.
#
#   encode-demo.sh --raw raw.mkv --events events.tsv --out lambdock-demo.gif
#
# The recording is inset in a rounded frame with a soft shadow (see
# make_overlay.py), captions are burnt in, and the whole thing is reduced to a
# 256-colour GIF with an error-diffusion dither that survives the palette drop.
# Re-encoding is cheap, so this is also the tool to use when tweaking the look
# of an existing recording: pass --raw and --events from a --keep run.
#
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

RAW=""
EVENTS=""
OUT=""
WIDTH="${WIDTH:-1100}"
FPS="${FPS:-20}"
MAX_MIB="${MAX_MIB:-12}"
DITHER="${DITHER:-sierra2_4a}"
CAPTIONS=1
KEEP=0

usage() {
  sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
  cat <<EOF

Options:
  --raw PATH        lossless recording (required)
  --events PATH     events.tsv written by controller.py (required)
  --out PATH        output GIF (required)
  --width N         output width; the screen inside the frame (default: $WIDTH)
  --fps N           GIF frame rate (default: $FPS)
  --max-mib N       size budget before extra compression is tried (default: $MAX_MIB)
  --dither NAME     paletteuse dither (default: $DITHER)
  --no-captions     leave the captions out (still keeps the frame)
  --keep            keep the presentation files for inspection
  -h, --help        this help
EOF
}

while (($#)); do
  case "$1" in
    --raw) RAW="$2"; shift 2 ;;
    --events) EVENTS="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --width) WIDTH="$2"; shift 2 ;;
    --fps) FPS="$2"; shift 2 ;;
    --max-mib) MAX_MIB="$2"; shift 2 ;;
    --dither) DITHER="$2"; shift 2 ;;
    --no-captions) CAPTIONS=0; shift ;;
    --keep) KEEP=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ -n "$RAW" && -n "$EVENTS" && -n "$OUT" ]] || { usage >&2; exit 2; }
[[ -s "$RAW" ]] || { echo "error: no recording at $RAW" >&2; exit 1; }
[[ -s "$EVENTS" ]] || { echo "error: no event log at $EVENTS" >&2; exit 1; }

note() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }

# ------------------------------------------------------------------ geometry
# The screen keeps the recording's aspect ratio; the frame margin scales with
# it so the chrome looks the same at any output width.
RAW_W="$(ffprobe -v error -select_streams v:0 -show_entries stream=width -of csv=p=0 "$RAW")"
RAW_H="$(ffprobe -v error -select_streams v:0 -show_entries stream=height -of csv=p=0 "$RAW")"
VIDEO_DURATION="$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$RAW")"
[[ -n "$RAW_W" && -n "$RAW_H" ]] || { echo "error: cannot probe $RAW" >&2; exit 1; }

# The recording may keep running for a moment after the storyboard ends (the
# recorder is stopped from outside), and any extra tail would be dead air.  Cut
# it at the last event plus a beat.
LAST_EVENT="$(awk -F'\t' 'NF >= 2 { if ($1 + 0 > last) last = $1 + 0 } END { print last + 0 }' "$EVENTS")"
DURATION="$(python3 -c "
import sys
video = float(sys.argv[1])
events = float(sys.argv[2])
print(f'{min(video, events + 1.4):.3f}')
" "$VIDEO_DURATION" "$LAST_EVENT")"
DURATION_INT="${DURATION%.*}"

SW=$(( WIDTH - (WIDTH % 2) ))
SH=$(( (RAW_H * SW / RAW_W) ))
(( SH % 2 )) && SH=$(( SH - 1 ))
MARGIN=$(( SW / 42 ))
(( MARGIN < 16 )) && MARGIN=16

TMP="$(mktemp -d "${TMPDIR:-/tmp}/lambdock-encode.XXXXXX")"
cleanup() { [[ "$KEEP" -eq 1 ]] && echo "kept $TMP" >&2 || rm -rf "$TMP"; }
trap cleanup EXIT

note "presentation ${SW}x${SH} + ${MARGIN}px frame, ${FPS} fps, ${DURATION_INT}s"
python3 "$SCRIPT_DIR/make_overlay.py" \
  --events "$EVENTS" --outdir "$TMP" --screen "${SW}x${SH}" \
  --margin "$MARGIN" --duration "$DURATION"

# ---------------------------------------------------------------------- video
# Three passes: composite (lossless), build one global palette, then map.
# Feeding palettegen and paletteuse from a single split in one command makes
# ffmpeg buffer the whole second branch in RAM -- that is an OOM kill on a
# recording this long, not a filter bug.
FILTER="[0:v]fps=${FPS},scale=${SW}:${SH}:flags=lanczos,setsar=1,format=rgba[rec]"
FILTER+=";[2:v]scale=${SW}:${SH}:flags=lanczos,format=gray[mask]"
FILTER+=";[rec][mask]alphamerge[screen]"
FILTER+=";[1:v]format=rgba[frame]"
FILTER+=";[frame][screen]overlay=0:0:format=auto:shortest=1[stage]"
if ((CAPTIONS)); then
  FILTER+=";[stage]subtitles=f=captions.ass:fontsdir=/usr/share/fonts[tk]"
else
  FILTER+=";[stage]null[tk]"
fi
FILTER+=";[tk]format=rgb24[v]"

note "compositing the frame and captions..."
mkdir -p "$(dirname -- "$OUT")"
(
  cd "$TMP"
  ffmpeg -y -loglevel error \
    -i "$RAW" \
    -loop 1 -framerate "$FPS" -i frame.png \
    -loop 1 -framerate "$FPS" -i mask.png \
    -filter_complex "$FILTER" -map "[v]" \
    -t "$DURATION" -c:v ffv1 -level 3 -pix_fmt rgb24 \
    "$TMP/composited.mkv"

  note "building the palette..."
  ffmpeg -y -loglevel error -i composited.mkv \
    -vf "palettegen=max_colors=256:stats_mode=full:reserve_transparent=0" \
    palette.png

  note "quantising to a GIF..."
  ffmpeg -y -loglevel error -i composited.mkv -i palette.png \
    -lavfi "paletteuse=dither=${DITHER}:diff_mode=rectangle" \
    -loop 0 "$TMP/out.gif"
)

# ---------------------------------------------------------------------- size
size="$(stat -c %s "$TMP/out.gif")"
max_bytes=$(( MAX_MIB * 1024 * 1024 ))
gifsicle -O3 --colors 256 -o "$TMP/opt.gif" "$TMP/out.gif"
size="$(stat -c %s "$TMP/opt.gif")"
if (( size > max_bytes )); then
  for lossy in 20 35 50 70; do
    note "$((size / 1024 / 1024)) MiB is over budget; trying gifsicle lossy=$lossy"
    gifsicle -O3 --lossy="$lossy" --colors 256 -o "$TMP/opt.gif" "$TMP/out.gif"
    size="$(stat -c %s "$TMP/opt.gif")"
    (( size <= max_bytes )) && break
  done
fi
if (( size > max_bytes )); then
  echo "error: still $((size / 1024 / 1024)) MiB at lossy compression;" >&2
  echo "       re-run with --width $((WIDTH - 100)) or --fps $((FPS - 4))" >&2
  exit 1
fi

cp "$TMP/opt.gif" "$OUT"
note "wrote $OUT ($((size / 1024)) KiB, $((SW + 2 * MARGIN))x$((SH + 2 * MARGIN)), ${FPS} fps, ${DURATION_INT}s)"
