#!/usr/bin/env bash
#
# Record a deterministic lambdock feature-and-theme showcase GIF.
#
# The script opens a *nested* niri session in the current niri session, gives it
# an isolated HOME/XDG_CONFIG_HOME and a private DBus session, runs lambdock with
# the demo configuration, drives the pointer through a virtual-pointer helper,
# records the output with wf-recorder, and turns the result into a GIF.
#
# To keep the host cursor out of the recording, the outer niri config is edited
# for the duration of the recording (cursor hide-after-inactive-ms = 100) and
# then restored byte-for-byte.  Pass --keep-cursor to skip that edit.
#
# Requirements: niri (wlr-virtual-pointer support), wf-recorder, ffmpeg with
# libass, gifsicle, wayland-scanner, a C compiler, python3 with numpy+Pillow,
# kitty, swaybg, dbus-run-session, jq and setsid.
#
#   ./tools/record-demo/record-demo.sh
#   ./tools/record-demo/record-demo.sh --out /tmp/lambdock.gif --keep
#
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
OUT_GIF="${OUT_GIF:-$SCRIPT_DIR/out/lambdock-demo.gif}"
KEEP_TMP="${KEEP_TMP:-0}"
CAPTIONS=1
FPS="${FPS:-15}"
WIDTH="${WIDTH:-1200}"
LOSSY="${LOSSY:-30}"
MAX_BYTES=$((15 * 1024 * 1024))
BUILD=0
HIDE_OUTER_CURSOR=1

usage() {
  sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
  cat <<EOF

Options:
  --out PATH        output GIF (default: $OUT_GIF)
  --keep            keep the temporary directory and raw recording
  --no-captions     do not burn captions into the GIF
  --width N         output width in pixels (default: $WIDTH)
  --fps N           GIF frame rate (default: $FPS)
  --lossy N         gifsicle lossy value (default: $LOSSY; raised if needed)
  --build           build/refresh build/lambdock before recording
  --keep-cursor     do not temporarily hide the host cursor
  -h, --help        this help
EOF
}

while (($#)); do
  case "$1" in
    --out) OUT_GIF="$2"; shift 2 ;;
    --keep) KEEP_TMP=1; shift ;;
    --no-captions) CAPTIONS=0; shift ;;
    --width) WIDTH="$2"; shift 2 ;;
    --fps) FPS="$2"; shift 2 ;;
    --lossy) LOSSY="$2"; shift 2 ;;
    --build) BUILD=1; shift ;;
    --keep-cursor) HIDE_OUTER_CURSOR=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

need() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "error: missing required command: $1" >&2
    exit 1
  fi
}

for command in niri wf-recorder ffmpeg gifsicle wayland-scanner gcc pkg-config \
               python3 kitty swaybg jq dbus-run-session setsid; do
  need "$command"
done
if ! pkg-config --exists wayland-client; then
  echo "error: wayland-client development files are required (pkg-config wayland-client)" >&2
  exit 1
fi
if ! python3 -c 'import numpy, PIL' >/dev/null 2>&1; then
  echo "error: python3 numpy and Pillow are required" >&2
  exit 1
fi
FFMPEG_FILTERS="$(ffmpeg -hide_banner -filters 2>/dev/null || true)"
if ! grep -q palettegen <<<"$FFMPEG_FILTERS"; then
  echo "error: this ffmpeg lacks the palettegen/paletteuse filters" >&2
  exit 1
fi
if ((CAPTIONS)) && ! grep -q ' subtitles' <<<"$FFMPEG_FILTERS"; then
  echo "warning: ffmpeg lacks the subtitles filter; captions disabled" >&2
  CAPTIONS=0
fi

if [[ "$(niri msg version 2>/dev/null || true)" == "" ]]; then
  echo "error: this script must run inside a niri session (niri msg failed)" >&2
  exit 1
fi

LAMBDOCK_BIN="${LAMBDOCK_BIN:-$ROOT/build/lambdock}"
if ((BUILD)); then
  need meson
  echo "building lambdock..."
  if [[ -d "$ROOT/build" ]]; then
    meson compile -C "$ROOT/build" >/dev/null
  else
    meson setup "$ROOT/build" "$ROOT" >/dev/null
    meson compile -C "$ROOT/build" >/dev/null
  fi
fi
if [[ ! -x "$LAMBDOCK_BIN" ]]; then
  echo "error: $LAMBDOCK_BIN not found; build it first or pass --build" >&2
  exit 1
fi

TMP="$(mktemp -d "${TMPDIR:-/tmp}/lambdock-demo.XXXXXX")"
mkdir -p "$TMP/assets" "$TMP/home/.config/lambdock" "$TMP/home/.local/share" \
         "$TMP/cache" "$TMP/vptr"
REC_PID=""
NESTED_GROUP=""
NIRI_CONFIG_PATH="${NIRI_CONFIG:-${XDG_CONFIG_HOME:-$HOME/.config}/niri/config.kdl}"
NIRI_CONFIG_BACKUP="$TMP/niri-config.backup"
NIRI_CONFIG_TOUCHED=0

restore_outer_cursor() {
  if [[ "$NIRI_CONFIG_TOUCHED" -eq 1 && -f "$NIRI_CONFIG_BACKUP" ]]; then
    cp "$NIRI_CONFIG_BACKUP" "$NIRI_CONFIG_PATH" 2>/dev/null || true
    NIRI_CONFIG_TOUCHED=0
    sleep 0.3
  fi
}

hide_outer_cursor() {
  [[ "$HIDE_OUTER_CURSOR" -eq 1 ]] || return 0
  if [[ ! -f "$NIRI_CONFIG_PATH" ]]; then
    echo "warning: no niri config at $NIRI_CONFIG_PATH; host cursor may be visible" >&2
    return 0
  fi
  cp "$NIRI_CONFIG_PATH" "$NIRI_CONFIG_BACKUP"
  if ! python3 - "$NIRI_CONFIG_PATH" <<'PY'
from pathlib import Path
import re
import sys

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
if "cursor {" in text:
    if "hide-after-inactive-ms" in text:
        text = re.sub(r"(hide-after-inactive-ms\s+)\d+", r"\g<1>100", text, count=1)
    else:
        text = text.replace("cursor {", "cursor {\n    hide-after-inactive-ms 100", 1)
else:
    text += "\ncursor {\n    hide-after-inactive-ms 100\n}\n"
path.write_text(text, encoding="utf-8")
PY
  then
    echo "warning: could not hide the host cursor; continuing" >&2
    cp "$NIRI_CONFIG_BACKUP" "$NIRI_CONFIG_PATH" 2>/dev/null || true
    return 0
  fi
  NIRI_CONFIG_TOUCHED=1
  sleep 1.5
}

cleanup() {
  trap - EXIT INT TERM
  if [[ -n "$REC_PID" ]]; then
    kill -INT "$REC_PID" 2>/dev/null || true
    wait "$REC_PID" 2>/dev/null || true
  fi
  if [[ -n "$NESTED_GROUP" ]]; then
    kill -TERM -- "-$NESTED_GROUP" 2>/dev/null || true
    sleep 0.4
    kill -KILL -- "-$NESTED_GROUP" 2>/dev/null || true
  fi
  restore_outer_cursor
  if [[ "$KEEP_TMP" -eq 1 ]]; then
    echo "kept temporary directory: $TMP" >&2
  else
    rm -rf "$TMP"
  fi
}
trap cleanup EXIT INT TERM

note() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
die() {
  printf '\033[1;31merror:\033[0m %s\n' "$*" >&2
  if [[ -f "$TMP/controller.log" ]]; then
    echo "--- controller.log (tail) ---" >&2
    tail -40 "$TMP/controller.log" >&2 || true
  fi
  exit 1
}

# ---------------------------------------------------------------- outer output
OUTPUT_NAME="${OUTPUT:-}"
outputs_json="$(niri msg -j outputs)"
if [[ -z "$OUTPUT_NAME" ]]; then
  OUTPUT_NAME="$(niri msg -j focused-output 2>/dev/null | jq -r '.name // empty' 2>/dev/null || true)"
fi
if [[ -z "$OUTPUT_NAME" ]]; then
  OUTPUT_NAME="$(jq -r 'keys_unsorted[0]' <<<"$outputs_json")"
fi
read -r OUTER_W OUTER_H OUTER_SCALE PHYS_W PHYS_H < <(
  jq -r --arg name "$OUTPUT_NAME" '
    .[$name] as $o
    | [
        $o.logical.width,
        $o.logical.height,
        $o.logical.scale,
        (($o.logical.width * $o.logical.scale) | round),
        (($o.logical.height * $o.logical.scale) | round)
      ] | @tsv
  ' <<<"$outputs_json"
)
[[ -n "$PHYS_W" ]] || die "could not read output geometry from niri"

NESTED_SCALE="${NESTED_SCALE:-}"
if [[ -z "$NESTED_SCALE" ]]; then
  if ((PHYS_W >= 2000)); then
    NESTED_SCALE=2.0
  elif ((PHYS_W >= 1500)); then
    NESTED_SCALE=1.5
  else
    NESTED_SCALE=1.0
  fi
fi
# Park the host pointer at (5,5) and remove a generous top/left margin from
# the recording.  We deliberately keep the right and bottom edges: the dock
# lives at the bottom edge and must not be cut.
EDGE="${EDGE:-64}"
CROP_W=$((PHYS_W - EDGE))
CROP_H=$((PHYS_H - EDGE))
if ((CROP_W < 640 || CROP_H < 360)); then
  EDGE=8
  CROP_W=$((PHYS_W - EDGE))
  CROP_H=$((PHYS_H - EDGE))
fi
if ((WIDTH > CROP_W)); then
  WIDTH=$CROP_W
fi
if ((WIDTH % 2)); then
  WIDTH=$((WIDTH - 1))
fi

note "outer output $OUTPUT_NAME: ${OUTER_W}x${OUTER_H} @ ${OUTER_SCALE}, physical ${PHYS_W}x${PHYS_H}"
note "nested output scale: $NESTED_SCALE (physical ${PHYS_W}x${PHYS_H})"

# ------------------------------------------------------------------- assets
note "generating wallpapers..."
python3 "$SCRIPT_DIR/make_wallpapers.py" "$TMP/assets" "$PHYS_W" "$PHYS_H"

REPL_SOCKET="$TMP/repl.sock"
REPL_FIFO="$TMP/repl.fifo"
REPL_DONE="$TMP/repl.done"
REPL_READY="$TMP/repl.ready"
mkfifo "$REPL_FIFO"

cat >"$TMP/home/.config/lambdock/settings.scm" <<EOF
;; Generated by tools/record-demo/record-demo.sh
(define dock-position 'bottom)
(define dock-theme 'vanilla)
(define dock-icon-size 48)
(define dock-auto-hide? #t)
(define dock-show-when-clear? #f)
(define dock-hover-animate? #t)
(define dock-track-windows? #t)
(define dock-enable-repl? #t)
(define dock-repl-socket-path "$REPL_SOCKET")
(define dock-exclude-apps '("kitty" "lambdock" "org.jointhefreeworld.lambdock"))
(define dock-items
  (append
   (preset-launchers-for 'alacritty 'google-chrome 'nautilus 'gnome-calculator)
   (list (separator-item))
   (preset-launchers-for 'emacs 'btop)
   (list (separator-item))
   (preset-launchers-for 'ram 'cpu 'clock)))
EOF

cat >"$TMP/niri.kdl" <<EOF
layout {
    gaps 4
    default-column-width { proportion 1.0; }
    focus-ring { off; }
    border { off; }
}
animations { off; }
prefer-no-csd
hotkey-overlay { skip-at-startup; }
output "winit" {
    scale $NESTED_SCALE
}
cursor {
    hide-after-inactive-ms 100
    xcursor-size 28
    xcursor-theme "Adwaita"
}
window-rule {
    match app-id="Alacritty"
    open-floating true
    default-column-width { fixed 760; }
    default-window-height { fixed 420; }
}
window-rule {
    match app-id="kitty"
    open-floating true
    default-column-width { fixed 1000; }
    default-window-height { fixed 500; }
}
window-rule {
    match app-id="org.gnome.Calculator"
    open-floating true
    default-column-width { fixed 420; }
    default-window-height { fixed 620; }
}
EOF
niri validate -c "$TMP/niri.kdl" >/dev/null 2>&1 || die "generated niri config is invalid"

# ----------------------------------------------------------- virtual pointer
note "building the virtual pointer helper..."
cp "$SCRIPT_DIR/vptr.c" "$SCRIPT_DIR/wlr-virtual-pointer-unstable-v1.xml" "$TMP/vptr/"
(
  cd "$TMP/vptr"
  wayland-scanner client-header wlr-virtual-pointer-unstable-v1.xml wlr-vptr.h
  wayland-scanner private-code wlr-virtual-pointer-unstable-v1.xml wlr-vptr.c
  gcc -O2 -Wall -Wextra -o vptr vptr.c wlr-vptr.c \
      $(pkg-config --cflags --libs wayland-client) -lm
)

# --------------------------------------------------------------- nested niri
note "starting nested niri..."
niri msg -j windows >"$TMP/outer-windows-before.json"

cat >"$TMP/run-nested.sh" <<'NESTED'
#!/bin/sh
pidfile=$1
niri_cfg=$2
controller=$3
tmp=$4
root=$5
lambdock=$6
settings=$7
socket=$8
fifo=$9
done_file=${10}
ready_file=${11}
echo "$$" >"$pidfile"
exec dbus-run-session -- niri -c "$niri_cfg" -- python3 "$controller" \
  --tmp "$tmp" \
  --root "$root" \
  --lambdock "$lambdock" \
  --settings "$settings" \
  --repl-socket "$socket" \
  --repl-fifo "$fifo" \
  --repl-done "$done_file" \
  --repl-ready "$ready_file"
NESTED
chmod +x "$TMP/run-nested.sh"

env -u NIRI_SOCKET \
    HOME="$TMP/home" \
    XDG_CONFIG_HOME="$TMP/home/.config" \
    XDG_DATA_HOME="$TMP/home/.local/share" \
    XDG_CACHE_HOME="$TMP/cache" \
    LANG=en_US.UTF-8 \
    LC_ALL=en_US.UTF-8 \
    OUTER_DISPLAY="$WAYLAND_DISPLAY" \
    OUTER_W="$OUTER_W" \
    OUTER_H="$OUTER_H" \
    OUTER_SCALE="$OUTER_SCALE" \
    NESTED_SCALE="$NESTED_SCALE" \
    DEMO_EDGE="$EDGE" \
    DEMO_PHYS_W="$PHYS_W" \
    DEMO_PHYS_H="$PHYS_H" \
    setsid --fork "$TMP/run-nested.sh" \
      "$TMP/session.pid" "$TMP/niri.kdl" "$SCRIPT_DIR/controller.py" \
      "$TMP" "$ROOT" "$LAMBDOCK_BIN" "$TMP/home/.config/lambdock/settings.scm" \
      "$REPL_SOCKET" "$REPL_FIFO" "$REPL_DONE" "$REPL_READY" \
    >"$TMP/nested.log" 2>&1 &

for _ in $(seq 1 300); do
  [[ -s "$TMP/session.pid" ]] && break
  sleep 0.1
done
if [[ ! -s "$TMP/session.pid" ]]; then
  die "nested niri did not start (see $TMP/nested.log)"
fi
NESTED_GROUP="$(cat "$TMP/session.pid")"

for _ in $(seq 1 300); do
  [[ -f "$TMP/nested-ready" ]] && break
  [[ -f "$TMP/error" ]] && die "nested session failed during setup"
  kill -0 "$NESTED_GROUP" 2>/dev/null || die "nested niri exited unexpectedly (see $TMP/nested.log)"
  sleep 0.1
done
[[ -f "$TMP/nested-ready" ]] || die "nested niri did not start"

NESTED_WIN=""
for _ in $(seq 1 100); do
  NESTED_WIN="$(niri msg -j windows | jq -r --slurpfile before "$TMP/outer-windows-before.json" '
    .[] | select(.app_id == "niri")
    | select((.id as $id | $before[0] | map(.id) | index($id)) == null)
    | .id
  ' | tail -1)"
  [[ -n "$NESTED_WIN" ]] && break
  sleep 0.1
done
[[ -n "$NESTED_WIN" ]] || die "could not find the nested niri window"

note "fullscreening nested window $NESTED_WIN..."
niri msg action focus-window --id "$NESTED_WIN"
niri msg action fullscreen-window --id "$NESTED_WIN"
sleep 0.9

touch "$TMP/fullscreen"

for _ in $(seq 1 300); do
  [[ -f "$TMP/ready" ]] && break
  [[ -f "$TMP/error" ]] && die "nested demo failed during setup"
  sleep 0.1
done
[[ -f "$TMP/ready" ]] || die "nested demo did not become ready"

# -------------------------------------------------------------- recording
note "hiding the host cursor during the recording..."
hide_outer_cursor

note "recording..."
# Let the controller know we are about to record.  The controller waits half a
# second before its first pointer move, so the recorder is already capturing by
# the time the auto-hide action starts.
touch "$TMP/go"
sleep 0.1

# The crop/scale is done while capturing, so the lossless intermediate stays
# small and the host cursor in the discarded top-left corner never reaches it.
wf-recorder -o "$OUTPUT_NAME" -r 20 -D \
  -F "crop=${CROP_W}:${CROP_H}:${EDGE}:${EDGE},scale=${WIDTH}:-2:flags=lanczos" \
  -f "$TMP/raw.mkv" >"$TMP/wf-recorder.log" 2>&1 &
REC_PID=$!
for _ in $(seq 1 100); do
  grep -qi 'recording' "$TMP/wf-recorder.log" && break
  kill -0 "$REC_PID" 2>/dev/null || die "wf-recorder exited (see $TMP/wf-recorder.log)"
  sleep 0.05
done

note "storyboard running..."
for _ in $(seq 1 1200); do
  [[ -f "$TMP/done" ]] && break
  [[ -f "$TMP/error" ]] && die "storyboard failed"
  kill -0 "$REC_PID" 2>/dev/null || die "wf-recorder stopped during the storyboard"
  sleep 0.1
done
[[ -f "$TMP/done" ]] || die "storyboard timed out"
sleep 0.4

kill -INT "$REC_PID" 2>/dev/null || true
wait "$REC_PID" 2>/dev/null || true
REC_PID=""
[[ -s "$TMP/raw.mkv" ]] || die "wf-recorder produced no video"

kill -TERM -- "-$NESTED_GROUP" 2>/dev/null || true
sleep 0.4
kill -KILL -- "-$NESTED_GROUP" 2>/dev/null || true
NESTED_GROUP=""

restore_outer_cursor

# ---------------------------------------------------------------- encode GIF
# wf-recorder already cropped and scaled the capture; only timing, captions
# and palette work are left.
RAW_DIM="$(ffprobe -v error -select_streams v:0 \
  -show_entries stream=width,height -of csv=s=x:p=0 "$TMP/raw.mkv")"
[[ -n "$RAW_DIM" ]] || die "could not read the recorded video dimensions"
RAW_W="${RAW_DIM%x*}"
RAW_H="${RAW_DIM#*x}"
FINAL_W=$RAW_W
FINAL_H=$RAW_H
note "encoding ${FINAL_W}x${FINAL_H} GIF at ${FPS} fps..."
FILTER="fps=${FPS}"
if ((CAPTIONS)); then
  python3 "$SCRIPT_DIR/make_subtitles.py" \
    "$TMP/captions.ass" "$TMP/events.tsv" "$FINAL_W" "$FINAL_H"
  FILTER+=",subtitles=captions.ass:fontsdir=/usr/share/fonts/truetype"
fi
FILTER+=",split[s0][s1];[s0]palettegen=max_colors=256:stats_mode=diff[p];[s1][p]paletteuse=dither=floyd_steinberg:diff_mode=rectangle"

mkdir -p "$(dirname "$OUT_GIF")"
(
  cd "$TMP"
  ffmpeg -y -loglevel error -i raw.mkv -vf "$FILTER" -loop 0 "$TMP/out.gif"
)

size=$(stat -c %s "$TMP/out.gif")
if ((size > MAX_BYTES)); then
  note "GIF is $((size / 1024 / 1024)) MiB; trying gifsicle lossy compression..."
  for lossy in "$LOSSY" 45 60 75 90 110 130; do
    gifsicle -O3 --lossy="$lossy" --colors 256 -o "$TMP/out-small.gif" "$TMP/out.gif"
    size=$(stat -c %s "$TMP/out-small.gif")
    if ((size <= MAX_BYTES)); then
      mv "$TMP/out-small.gif" "$TMP/out.gif"
      break
    fi
  done
else
  gifsicle -O3 --lossy="$LOSSY" --colors 256 -o "$TMP/out-small.gif" "$TMP/out.gif"
  mv "$TMP/out-small.gif" "$TMP/out.gif"
  size=$(stat -c %s "$TMP/out.gif")
fi

if ((size > MAX_BYTES)); then
  die "could not get the GIF under 15 MiB; try --width 1000 or --fps 12"
fi

mv "$TMP/out.gif" "$OUT_GIF"
note "wrote $OUT_GIF ($((size / 1024)) KiB, ${FINAL_W}x${FINAL_H}, ${FPS} fps)"
