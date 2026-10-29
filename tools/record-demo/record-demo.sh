#!/usr/bin/env bash
# lambdock -- hyper-hackable desktop dock for Wayland
#
# SPDX-License-Identifier: MIT
# Copyright © Josep Bigorra <jjbigorra@gmail.com>

#
# Record a deterministic lambdock feature-and-theme showcase GIF.
#
# The script opens a *nested* niri session in the current niri session, gives it
# an isolated HOME/XDG_CONFIG_HOME and a private DBus session, runs lambdock with
# the demo configuration, drives the dock through a virtual pointer, records the
# output with wf-recorder, and hands the recording to encode-demo.sh.
#
# The host is never reconfigured: the pointer that drives the demo belongs to
# the nested compositor, and the host pointer is parked in the margin that the
# capture crops away.
#
# Requirements: niri (wlr-virtual-pointer support), wf-recorder, ffmpeg,
# gifsicle, wayland-scanner, a C compiler, python3 with numpy+Pillow, kitty,
# alacritty, swaybg, dbus-run-session, jq and setsid.
#
#   ./tools/record-demo/record-demo.sh
#   ./tools/record-demo/record-demo.sh --out /tmp/lambdock.gif --keep
#
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
OUT_GIF="${OUT_GIF:-$SCRIPT_DIR/out/lambdock-demo.gif}"
KEEP_TMP="${KEEP_TMP:-0}"
# --width and --fps only affect the encode: the pointer coordinates are in the
# nested *logical* space, so a smaller or faster GIF needs no recalibration.
FPS="${FPS:-20}"
WIDTH="${WIDTH:-1100}"
# The lossless intermediate is captured at this width; the GIF is scaled down
# from it, which keeps text crisp after the palette reduction.
CAPTURE_WIDTH="${CAPTURE_WIDTH:-1440}"
BUILD=0
HOST_CURSOR_EDIT=1

usage() {
  sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'
  cat <<EOF

Options:
  --out PATH        output GIF (default: $OUT_GIF)
  --keep            keep the temporary directory and raw recording
  --width N         GIF screen width in pixels (default: $WIDTH)
  --fps N           GIF frame rate (default: $FPS)
  --capture-width N width of the lossless capture (default: $CAPTURE_WIDTH)
  --build           build/refresh build/lambdock before recording
  --keep-cursor     do not touch the host niri config at all
  -h, --help        this help
EOF
}

while (($#)); do
  case "$1" in
    --out) OUT_GIF="$2"; shift 2 ;;
    --keep) KEEP_TMP=1; shift ;;
    --width) WIDTH="$2"; shift 2 ;;
    --fps) FPS="$2"; shift 2 ;;
    --capture-width) CAPTURE_WIDTH="$2"; shift 2 ;;
    --build) BUILD=1; shift ;;
    --keep-cursor) HOST_CURSOR_EDIT=0; shift ;;
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

for command in niri wf-recorder ffmpeg ffprobe gifsicle wayland-scanner gcc \
               pkg-config python3 kitty alacritty swaybg jq dbus-run-session setsid; do
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
if ! grep -q ' subtitles' <<<"$FFMPEG_FILTERS"; then
  echo "error: this ffmpeg lacks the subtitles filter (libass)" >&2
  exit 1
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
    "$TMP/home/.local/share/icons" "$TMP/home/demo" "$TMP/bin" "$TMP/cache" \
    "$TMP/vptr" "$TMP/state" "$TMP/shots"
REC_PID=""
NESTED_GROUP=""
NIRI_CONFIG_PATH="${NIRI_CONFIG:-${XDG_CONFIG_HOME:-$HOME/.config}/niri/config.kdl}"
NIRI_CONFIG_BACKUP="$TMP/niri-config.backup"
NIRI_CONFIG_TOUCHED=0

# The pointer the demo drives is a *virtual* pointer, so it is the host's
# hardware cursor that ends up in the recording: it is the only cursor there is
# (the nested compositor is given a transparent cursor theme below), and it
# must not vanish during the pauses in the storyboard.  Ask the host compositor
# to keep it, for the duration of the recording only, and put the file back byte
# for byte on the way out.
restore_host_cursor() {
  if [[ "$NIRI_CONFIG_TOUCHED" -eq 1 && -f "$NIRI_CONFIG_BACKUP" ]]; then
    cp "$NIRI_CONFIG_BACKUP" "$NIRI_CONFIG_PATH" 2>/dev/null || true
    NIRI_CONFIG_TOUCHED=0
    sleep 0.3
  fi
}

keep_host_cursor_visible() {
  [[ "$HOST_CURSOR_EDIT" -eq 1 ]] || return 0
  if [[ ! -f "$NIRI_CONFIG_PATH" ]]; then
    echo "warning: no niri config at $NIRI_CONFIG_PATH; the host cursor may hide" >&2
    return 0
  fi
  cp "$NIRI_CONFIG_PATH" "$NIRI_CONFIG_BACKUP"
  if ! python3 - "$NIRI_CONFIG_PATH" <<'PY'
from pathlib import Path
import re
import sys

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
value = "hide-after-inactive-ms 3600000"
if "cursor {" in text:
    if "hide-after-inactive-ms" in text:
        text = re.sub(r"hide-after-inactive-ms\s+\d+", value, text, count=1)
    else:
        text = text.replace("cursor {", "cursor {\n    " + value, 1)
else:
    text += "\ncursor {\n    " + value + "\n}\n"
path.write_text(text, encoding="utf-8")
PY
  then
    echo "warning: could not adjust the host cursor; continuing" >&2
    cp "$NIRI_CONFIG_BACKUP" "$NIRI_CONFIG_PATH" 2>/dev/null || true
    return 0
  fi
  NIRI_CONFIG_TOUCHED=1
  # niri reloads its config on write.
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
  restore_host_cursor
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

# The capture drops a margin from the *top* only: the dock owns the bottom edge,
# and a taskbar spans the full width, so cutting the sides would clip the clock
# in taskbar mode.  The margin is where the pointer is parked before the
# storyboard starts, so the cursor never enters the recording from there.
EDGE="${EDGE:-56}"
(( EDGE % 2 )) && EDGE=$(( EDGE + 1 ))
CROP_W=$(( PHYS_W ))
CROP_H=$(( PHYS_H - EDGE ))
CROP_X=0
if (( CROP_W < 640 || CROP_H < 360 )); then
  EDGE=8
  CROP_H=$(( PHYS_H - EDGE ))
fi
if (( CAPTURE_WIDTH > CROP_W )); then
  CAPTURE_WIDTH=$CROP_W
fi
(( CAPTURE_WIDTH % 2 )) && CAPTURE_WIDTH=$(( CAPTURE_WIDTH - 1 ))

note "outer output $OUTPUT_NAME: ${OUTER_W}x${OUTER_H} @ ${OUTER_SCALE}, physical ${PHYS_W}x${PHYS_H}"
note "nested output scale: $NESTED_SCALE; capture ${CAPTURE_WIDTH}px wide"

# ------------------------------------------------------------------- assets
note "generating wallpapers..."
python3 "$SCRIPT_DIR/make_wallpapers.py" "$TMP/assets" "$PHYS_W" "$PHYS_H"

# A folder for the folder-stack scene: the wallpapers plus a couple of recolours,
# so the stack popover has a grid worth browsing.
mkdir -p "$TMP/shots"
cp "$TMP/assets/galaxy.png" "$TMP/shots/01-galaxy.png"
cp "$TMP/assets/mountains.png" "$TMP/shots/02-sunrise.png"
if command -v magick >/dev/null 2>&1; then
  magick "$TMP/assets/galaxy.png" -modulate 100,120,82 "$TMP/shots/03-nebula.png"
  magick "$TMP/assets/mountains.png" -modulate 94,108,118 "$TMP/shots/04-dusk.png"
fi

# Files for the demo terminals to live in (the prompt shows ~/demo).
printf '%s\n' \
  ';; the settings file the dock is running right now' \
  '(define dock-icon-size 48)' >"$TMP/home/demo/lambdock.scm"
printf '%s\n' \
  '# lambdock notes' \
  '' '- the dock is a Scheme program' \
  '- reload-dock! re-reads this file' >"$TMP/home/demo/notes.org"
printf '%s\n' 'Two things are endless: the universe and the dock config.' \
  >"$TMP/home/demo/README.md"

# The terminals in the demo run this instead of the user's shell, so their
# content is deterministic and worth looking at.  It is only ever on $PATH for
# the nested session.
cat >"$TMP/bin/lambdock-demo-shell" <<'SHELL'
#!/bin/sh
# lambdock -- demo shell for the showcase recording
mode="${1:-prompt}"
printf '\033[1;38;5;183m  lambdock\033[0m \033[38;5;245mdemo shell\033[0m\n\n'
case "$mode" in
  fetch)
    fastfetch --logo small --structure os:wm:de:terminal:cpu:memory
    ;;
  files)
    printf '\033[1;38;5;114m~\033[0m \033[38;5;245mthe settings the dock is running\033[0m\n\n'
    ls -1 --color=always --group-directories-first
    printf '\n\033[38;5;245mreload-dock! re-reads these while it runs\033[0m\n'
    ;;
esac
exec bash -i
SHELL
chmod +x "$TMP/bin/lambdock-demo-shell"

cat >"$TMP/home/.bashrc" <<'RC'
# lambdock demo shell: a quiet prompt, no user configuration
PS1='\[\e[38;5;114m\]➜\[\e[0m\] \[\e[38;5;110m\]\w\[\e[0m\] '
export PS1
export HISTFILE=/dev/null
RC
cp "$TMP/home/.bashrc" "$TMP/home/.profile"

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
(define dock-capsule-padding 18)
(define dock-track-windows? #t)
(define dock-enable-repl? #t)
(define dock-repl-socket-path "$REPL_SOCKET")
(define dock-exclude-apps '("kitty" "lambdock" "org.jointhefreeworld.lambdock"))
(define dock-items
  (append
   (preset-launchers-for 'alacritty 'google-chrome 'nautilus 'gnome-calculator)
   (list (separator-item))
   (list (preset-launcher 'emacs)
         (stack-item #:name "Wallpapers" #:icon "folder"
                     #:path "$TMP/shots" #:view-mode 'grid))
   (list (separator-item))
   (preset-launchers-for 'ram 'cpu 'clock)))
EOF

cat >"$TMP/niri.kdl" <<EOF
layout {
    gaps 4
    default-column-width { proportion 1.0; }
    focus-ring { off; }
    border { off; }
    /* Fallback backdrop: the wallpaper is drawn by swaybg, and if it ever
       fails to map the demo should still look deliberate. */
    background-color "#0b0d16"
}
prefer-no-csd
hotkey-overlay { skip-at-startup; }
output "winit" {
    scale $NESTED_SCALE
}
cursor {
    /* The nested compositor's cursor is never drawn: the recording shows the
       host's cursor, which is the one the virtual pointer actually moves.  A
       transparent theme is the only way to say that. */
    hide-after-inactive-ms 3600000
    xcursor-size 18
    xcursor-theme "lambdock-empty"
}
window-rule {
    match app-id="Alacritty"
    open-floating true
    default-column-width { fixed 470; }
    default-window-height { fixed 330; }
}
window-rule {
    match app-id="kitty"
    open-floating true
    default-column-width { fixed 900; }
    default-window-height { fixed 430; }
}
/* The Preferences dialog is the subject of its own scene.  Opening it floating
   at its intended size keeps it from tiling full width and then being resized
   on camera, which is what made the first frames of that scene look wrong. */
window-rule {
    match app-id="org.jointhefreeworld.lambdock" title="Lambdock Preferences"
    open-floating true
    default-column-width { fixed 620; }
    default-window-height { fixed 548; }
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

note "writing the transparent cursor theme for the nested session..."
python3 "$SCRIPT_DIR/make_empty_cursor.py" \
  "$TMP/home/.local/share/icons/lambdock-empty" >/dev/null

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
    XDG_STATE_HOME="$TMP/state" \
    SHELL="$TMP/bin/lambdock-demo-shell" \
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
note "keeping the host cursor visible for the recording..."
keep_host_cursor_visible

# The crop/scale happens while capturing, so the lossless intermediate is
# already the right shape and the margins never reach it.
note "recording..."
wf-recorder -o "$OUTPUT_NAME" -r 20 -D \
  -F "crop=${CROP_W}:${CROP_H}:${CROP_X}:${EDGE},scale=${CAPTURE_WIDTH}:-2:flags=lanczos" \
  -f "$TMP/raw.mkv" >"$TMP/wf-recorder.log" 2>&1 &
REC_PID=$!

# Wait for frames, not for a log line: the storyboard must not start a second
# before video time zero, or every caption would land early.
previous=0
for _ in $(seq 1 600); do
  kill -0 "$REC_PID" 2>/dev/null || die "wf-recorder exited (see $TMP/wf-recorder.log)"
  size="$(stat -c %s "$TMP/raw.mkv" 2>/dev/null || echo 0)"
  if (( size > previous && size > 0 )); then
    break
  fi
  previous="$size"
  sleep 0.05
done

# Only now does the storyboard start, which is what keeps the burnt-in captions
# in sync with the picture: video time zero is a fraction of a second before
# event time zero.
touch "$TMP/go"

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

restore_host_cursor

# ---------------------------------------------------------------- encode GIF
"$SCRIPT_DIR/encode-demo.sh" \
  --raw "$TMP/raw.mkv" --events "$TMP/events.tsv" --out "$OUT_GIF" \
  --width "$WIDTH" --fps "$FPS"

note "events: $TMP/events.tsv"
