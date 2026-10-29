#!/usr/bin/env python3
# lambdock -- hyper-hackable desktop dock for Wayland
#
# SPDX-License-Identifier: MIT
# Copyright © Josep Bigorra <jjbigorra@gmail.com>

"""Drive the lambdock showcase storyboard inside a nested niri session.

This program is started by niri as the nested session's startup command, so it
inherits WAYLAND_DISPLAY, NIRI_SOCKET, HOME and the private DBus session created
by record-demo.sh.  It sets up the wallpaper, the dock and a virtual pointer,
waits for the outer script to say "go", then performs the demo with explicit
sleeps and coordinates.  Every scene is timestamped in events.tsv so the encoder
can burn matching captions into the final GIF.

Two pointer decisions are worth knowing about:

* The pointer that drives the demo is a virtual pointer bound to the *nested*
  compositor.  It moves that compositor's own cursor, so the recording shows a
  single, real, always-accurate cursor -- no cursor is drawn in post.
* The *outer* pointer is parked once at (5,5), inside the margin the recorder
  crops away, so the host compositor's hardware cursor never reaches the GIF
  and no host configuration is touched.

Nothing here writes outside $TMP except through the compositor.
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import signal
import socket
import subprocess
import sys
import time
import traceback
from pathlib import Path

# The storyboard is authored against a 1128x752 logical output (2256x1504
# physical at scale 2).  The dock is anchored to the bottom edge, so another
# output size only needs a translation.
REF_W = 1128
REF_H = 752

# Dock item centres, in the coordinates above.  Measured from the reference
# recording with tools/record-demo/measure_dock.py; the settings file this
# recorder generates is what fixes them.  Re-measured for the analogue clock:
# the clock item used to be a time string, and a face is a cell narrower, which
# moves every item before it (the dock is centred).
ITEM_ALACRITTY = 278
ITEM_CHROME = 343
ITEM_NAUTILUS = 407
ITEM_CALCULATOR = 470
ITEM_SEPARATOR = 517
ITEM_EMACS = 564
ITEM_STACK = 628
ITEM_SEPARATOR_2 = 674
ITEM_RAM = 720
ITEM_CPU = 785
DOCK_ROW = 708

# Popover rows hang above the dock at fixed offsets from the bottom, so the
# last two rows do not move with the number of windows.
POPOVER_CLOSE_ALL = 594

# resources/css/macos-mavericks.css is *material*, not geometry: the 3D slab with
# icons mirrored onto the glass in front of them only appears with the three dock
# options the stylesheet documents (shelf margin -22, shelf front 18, reflection
# 0.5).  Everything else in the montage wants the stock flat capsule back.
#
# Theme and geometry have to travel in *one* expression: reload-dock! only
# queues a redraw, so a profile change issued between two reloads can land in
# the same redraw as the next theme and paint the wrong pair.  That is why the
# shelf geometry is typed as part of the swap helper instead of being set
# between themes, and why the reset below is one expression as well.
SHELF_OFF = "(set-dock-shelf-margin! 24) (set-dock-shelf-front! 0) (set-dock-reflection! 0.0)"
# Typed into the dock's REPL: swap a theme, and take the shelf geometry with it
# when a second argument is given.  Short on purpose -- it is on screen.
SWAP_DEFINITION = (
    "(define (swap! t . shelf) (set-dock-theme! t) "
    "(let ((s (pair? shelf))) "
    "(set-dock-shelf-margin! (if s -22 24)) "
    "(set-dock-shelf-front! (if s 18 0)) "
    "(set-dock-reflection! (if s 0.5 0))) (reload-dock!))"
)
# The shelf's capsule hangs lower than the flat one's, so the parked pointer
# sits low enough to be inside both.
MONTAGE_ROW = 736


THEME_LABELS = {
    "vanilla": "Vanilla Glassmorphism",
    "macos-big-sur": "macOS Big Sur",
    "macos-ventura": "macOS Ventura",
    "macos-mavericks": "macOS Mavericks",
    "macos-mavericks-dark": "macOS Mavericks Dark",
    "unity": "Unity",
    "nature": "Nature",
    "snow-leopard-dark": "Snow Leopard Dark",
    "windows-10": "Windows 10",
    "windows-11": "Windows 11",
    "vanilla-light": "Vanilla Light",
    "snow-leopard": "Snow Leopard 3D Glass",
    "solarized-light": "Solarized Light",
    "windows-xp": "Windows XP Luna",
    "none": "No theme (GTK default)",
}

DARK_THEMES = [
    "macos-big-sur",
    "macos-ventura",
    "unity",
    "snow-leopard-dark",
    "windows-11",
    "macos-mavericks-dark",
]
LIGHT_THEMES = [
    "vanilla-light",
    "snow-leopard",
    "solarized-light",
    "windows-xp",
    "macos-mavericks",
]


class Demo:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.tmp = Path(args.tmp)
        self.events_path = self.tmp / "events.tsv"
        self.log_path = self.tmp / "controller.log"
        self.events_file = None
        self.t0 = 0.0
        self.vptr = None
        self.repl_fd: int | None = None
        self.repl_index = 0
        self.repl_done = self.tmp / "repl.done"
        self.wallpaper_pid_file = self.tmp / "swaybg.pid"
        self.pointer_x = 0.0
        self.pointer_y = 0.0
        self.out_w = REF_W
        self.out_h = REF_H

    # ------------------------------------------------------------------ io
    def log(self, message: str) -> None:
        line = f"[controller] {message}"
        print(line, flush=True)
        with open(self.log_path, "a", encoding="utf-8") as handle:
            handle.write(line + "\n")

    def write_atomic(self, path: Path, value: str) -> None:
        tmp = path.with_suffix(path.suffix + ".tmp")
        tmp.write_text(value, encoding="utf-8")
        os.replace(tmp, path)

    def wait_for(self, path: Path, timeout: float = 30.0) -> None:
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if path.exists():
                return
            error = self.tmp / "error"
            if error.exists():
                raise RuntimeError(f"outer script failed: {error.read_text()}")
            time.sleep(0.05)
        raise TimeoutError(f"timed out waiting for {path}")

    # --------------------------------------------------------------- niri
    def niri(self, *args: str) -> str:
        result = subprocess.run(
            ["niri", "msg", *args],
            check=True,
            capture_output=True,
            text=True,
            env=os.environ,
        )
        return result.stdout

    def niri_json(self, *args: str):
        return json.loads(self.niri(*args))

    def spawn_sh(self, script: str) -> None:
        self.niri("action", "spawn-sh", "--", script)

    def windows(self) -> list[dict]:
        return self.niri_json("-j", "windows")

    def find_window(self, **matches) -> dict | None:
        for window in self.windows():
            if all(window.get(key) == value for key, value in matches.items()):
                return window
        return None

    def wait_window(self, timeout: float = 10.0, **matches) -> dict | None:
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            window = self.find_window(**matches)
            if window:
                return window
            time.sleep(0.08)
        return None

    def close_window(self, window_id: int) -> None:
        try:
            self.niri("action", "close-window", "--id", str(window_id))
        except subprocess.CalledProcessError:
            pass

    def place(self, window_id: int, x: int, y: int, width: int, height: int) -> None:
        """Pin a floating window to an exact screen rectangle."""
        for action in (
            ("set-window-width", "--id", str(window_id), str(width)),
            ("set-window-height", "--id", str(window_id), str(height)),
            ("move-floating-window", "--id", str(window_id), "--x", str(x), "--y", str(y)),
        ):
            try:
                self.niri("action", *action)
            except subprocess.CalledProcessError as error:
                self.log(f"{action[0]} failed: {error}")

    # ---------------------------------------------------------- coordinates
    def X(self, reference: float) -> int:
        return int(round(reference + (self.out_w - REF_W) / 2.0))

    def Y(self, reference: float) -> int:
        return int(round(reference + (self.out_h - REF_H)))

    # -------------------------------------------------------------- events
    def event(self, kind: str, text: str = "") -> None:
        if self.events_file is None:
            return
        elapsed = time.monotonic() - self.t0
        self.events_file.write(f"{elapsed:.3f}\t{kind}\t{text}\n")
        self.events_file.flush()

    def caption(self, text: str) -> None:
        self.event("caption_start", text)

    def caption_end(self) -> None:
        self.event("caption_end")

    def theme(self, name: str) -> None:
        self.event("theme", THEME_LABELS.get(name, name))

    def theme_end(self) -> None:
        self.event("theme_end")

    def title(self, text: str, subtitle: str = "") -> None:
        self.event("title", f"{text}\t{subtitle}")

    def title_end(self) -> None:
        self.event("title_end")

    # ---------------------------------------------------------- vptr driver
    def vptr_path(self) -> str:
        return str(self.tmp / "vptr" / "vptr")

    def start_vptr(self) -> None:
        """Bind a virtual pointer to the *host* compositor.

        The nested compositor ignores a virtual pointer of its own when it
        comes to pointer focus: motion from it is delivered to whatever surface
        already has focus, so a hidden dock never sees the pointer reach its
        reveal strip and the fish-eye never starts.  Driving the host pointer
        instead makes the nested compositor see an ordinary pointer crossing
        its window, which is what a real user does -- and because a virtual
        pointer does not move the host's hardware cursor, the only cursor in
        the recording is the nested compositor's own.
        """
        self.outer_display = os.environ.get("OUTER_DISPLAY", "")
        self.outer_w = int(os.environ.get("OUTER_W", self.out_w) or self.out_w)
        self.outer_h = int(os.environ.get("OUTER_H", self.out_h) or self.out_h)
        self.outer_scale = float(os.environ.get("OUTER_SCALE", "1") or 1)
        self.nested_scale = float(os.environ.get("NESTED_SCALE", "1") or 1)
        if not self.outer_display:
            raise RuntimeError("OUTER_DISPLAY is not set; cannot drive the pointer")

        env = dict(os.environ)
        env["WAYLAND_DISPLAY"] = self.outer_display
        self.vptr = subprocess.Popen(
            [self.vptr_path(), str(self.outer_w), str(self.outer_h)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            env=env,
        )
        assert self.vptr.stdout is not None
        ready = self.vptr.stdout.readline().strip()
        if not ready.startswith("READY"):
            raise RuntimeError(f"vptr failed to start: {ready!r}")
        self.log(
            f"virtual pointer ready on the host {self.outer_w}x{self.outer_h} "
            f"(host scale {self.outer_scale}, nested scale {self.nested_scale})"
        )

    def vp(self, command: str) -> None:
        assert self.vptr is not None and self.vptr.stdin is not None
        elapsed = time.monotonic() - self.t0 if self.t0 else 0.0
        self.log(f"vp {elapsed:6.2f} {command}")
        self.vptr.stdin.write(command + "\n")
        self.vptr.stdin.flush()

    def to_outer(self, x: float, y: float) -> tuple[float, float]:
        """Nested logical coordinates -> host logical coordinates.

        The nested window is a rectangle on the host output, and both
        compositors have their own scale factor.
        """
        factor = self.nested_scale / self.outer_scale
        return x * factor, y * factor

    def move(self, x: float, y: float) -> None:
        ox, oy = self.to_outer(x, y)
        self.vp(f"move {ox:.0f} {oy:.0f}")
        self.pointer_x, self.pointer_y = x, y
        time.sleep(0.05)

    def glide(self, x: float, y: float, ms: int) -> None:
        ox, oy = self.to_outer(x, y)
        self.vp(f"glide {ox:.0f} {oy:.0f} {ms}")
        self.pointer_x, self.pointer_y = x, y
        time.sleep(ms / 1000.0 + 0.03)

    def click(self) -> None:
        self.vp("click")
        time.sleep(0.1)

    def right_click(self) -> None:
        self.vp("rightclick")
        time.sleep(0.1)

    def park(self) -> None:
        self.move(6, 6)

    def reveal_dock(self, x: float | None = None) -> None:
        """Bring an auto-hidden (or un-tucked) dock back on screen.

        Touching the strip at the bottom edge is what a user does, and it is
        also the only way to get the dock out of the "hidden" state it can be
        left in after a reload.
        """
        target = self.X(ITEM_ALACRITTY) if x is None else x
        self.glide(self.out_w / 2, self.out_h - 1, 240)
        time.sleep(0.6)
        self.glide(target, self.Y(DOCK_ROW), 280)
        time.sleep(0.3)

    # ---------------------------------------------------------- setup
    def setup(self) -> None:
        self.wait_for(self.tmp / "fullscreen")
        outputs = self.niri_json("-j", "outputs")
        output = next(iter(outputs.values()))
        self.out_w = int(output["logical"]["width"])
        self.out_h = int(output["logical"]["height"])
        self.log(f"nested output is {self.out_w}x{self.out_h}")

        self.start_swaybg("galaxy.png")
        self.spawn_sh(
            "cd "
            + shlex.quote(self.args.root)
            + " && exec "
            + shlex.quote(self.args.lambdock)
            + " >>"
            + shlex.quote(str(self.tmp / "lambdock.log"))
            + " 2>&1"
        )

        end = time.monotonic() + 20.0
        while time.monotonic() < end:
            if 'Namespace: "lambdock"' in self.niri("layers"):
                break
            time.sleep(0.1)
        else:
            raise RuntimeError("lambdock never appeared in the layer list")

        # schedule_initial_autohide fires after 1s; give the slide animation
        # time to finish before the recording starts.
        time.sleep(2.0)

        self.start_vptr()
        self.park()
        self.write_atomic(self.tmp / "ready", "1")
        self.log("nested session ready")

    def start_swaybg(self, image_name: str) -> None:
        image = self.tmp / "assets" / image_name
        pid_file = self.wallpaper_pid_file
        script = (
            f"echo $$ > {shlex.quote(str(pid_file))}; "
            f"exec swaybg -i {shlex.quote(str(image))} -m fill"
        )
        # swaybg occasionally exits on its first frame (a compositor/GL race on
        # a fresh nested session), which would leave the recording on niri's
        # fallback colour.  Start it again if it is not alive a moment later.
        for attempt in range(3):
            pid_file.unlink(missing_ok=True)
            self.spawn_sh(script)
            end = time.monotonic() + 5.0
            pid = None
            while time.monotonic() < end:
                if pid_file.exists():
                    try:
                        pid = int(pid_file.read_text().strip())
                    except ValueError:
                        pid = None
                    if pid:
                        break
                time.sleep(0.05)
            if pid is None:
                self.log(f"swaybg ({image_name}) never reported a pid")
                continue
            time.sleep(0.6)
            try:
                os.kill(pid, 0)
            except OSError:
                self.log(f"swaybg ({image_name}) exited; attempt {attempt + 1}")
                continue
            time.sleep(0.2)
            return
        raise RuntimeError(f"could not start swaybg for {image_name}")

    def switch_wallpaper(self, image_name: str) -> None:
        """Swap the wallpaper without a black flash.

        The new swaybg gets its own layer surface, which the compositor stacks
        above the old one, so the picture changes the moment the old instance
        exits instead of the moment the new one starts.
        """
        try:
            old_pid = int(self.wallpaper_pid_file.read_text().strip())
        except (OSError, ValueError):
            old_pid = 0
        self.wallpaper_pid_file.unlink(missing_ok=True)
        self.start_swaybg(image_name)
        if old_pid:
            try:
                os.kill(old_pid, signal.SIGTERM)
            except OSError:
                pass
        time.sleep(0.3)

    # ---------------------------------------------------------- REPL
    def dock_eval(self, expression: str, timeout: float = 4.0) -> None:
        """Evaluate one expression over a short-lived REPL connection."""
        prompt = b"scheme@(guile-user)> "
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.settimeout(timeout)
            sock.connect(self.args.repl_socket)
            time.sleep(0.1)
            data = b""
            while prompt not in data:
                chunk = sock.recv(65536)
                if not chunk:
                    break
                data += chunk
            sock.sendall(expression.encode() + b"\n")
            data = b""
            while prompt not in data:
                chunk = sock.recv(65536)
                if not chunk:
                    break
                data += chunk
            sock.close()
        except OSError as error:
            self.log(f"dock_eval failed ({error}): {expression}")

    def launch_repl_window(self) -> int:
        repl_ui = Path(self.args.root) / "tools" / "record-demo" / "repl_ui.py"
        command = (
            "exec kitty --title "
            + shlex.quote("lambdock REPL")
            + " -o font_size=13 -o background_opacity=0.94"
            + " -o hide_window_decorations=yes"
            + " -o confirm_os_window_close=0"
            + " -o window_padding_width=16 python3 "
            + shlex.quote(str(repl_ui))
            + " --socket "
            + shlex.quote(self.args.repl_socket)
            + " --fifo "
            + shlex.quote(self.args.repl_fifo)
            + " --done "
            + shlex.quote(self.args.repl_done)
            + " --ready "
            + shlex.quote(self.args.repl_ready)
        )
        self.spawn_sh(command)

        window = self.wait_window(title="lambdock REPL", timeout=15.0)
        if not window:
            raise RuntimeError("kitty did not open")

        window_id = int(window["id"])
        try:
            self.niri("action", "move-window-to-floating", "--id", str(window_id))
        except subprocess.CalledProcessError:
            pass
        self.place(window_id, 96, 104, 940, 430)
        self.wait_for(Path(self.args.repl_ready), timeout=15.0)
        time.sleep(0.3)
        self.log(f"REPL terminal ready (window {window_id})")
        return window_id

    def open_repl_writer(self) -> None:
        # Opening the FIFO blocks until repl_ui opens its read end.
        self.repl_fd = os.open(self.args.repl_fifo, os.O_WRONLY)

    def repl(self, expression: str, speed_ms: int = 12, hold: float = 0.0) -> None:
        assert self.repl_fd is not None
        self.repl_index += 1
        os.write(self.repl_fd, f"{speed_ms}\t{expression}\n".encode())
        end = time.monotonic() + 10.0
        while time.monotonic() < end:
            try:
                done = int(self.repl_done.read_text().strip())
            except (OSError, ValueError):
                done = -1
            if done >= self.repl_index:
                break
            time.sleep(0.03)
        else:
            # One slow reload is not worth throwing away the recording: log it
            # and let the next expression resync.
            self.log(f"REPL command timed out (continuing): {expression}")
        if hold:
            time.sleep(hold)

    # ---------------------------------------------------------- terminals
    def demo_shell(self) -> str:
        return str(self.tmp / "bin" / "lambdock-demo-shell")

    def spawn_terminal(
        self, title: str, mode: str, x: int, y: int, width: int, height: int
    ) -> int | None:
        """Open a floating terminal running the demo shell in @mode."""
        command = (
            "cd "
            + shlex.quote(str(self.tmp / "home" / "demo"))
            + " && exec alacritty --title "
            + shlex.quote(title)
            + " -o window.dynamic_title=false -o window.decorations=none"
            + " -o font.size=9"
            + " -e "
            + shlex.quote(self.demo_shell())
            + " "
            + shlex.quote(mode)
        )
        self.spawn_sh(command)
        window = self.wait_window(title=title, timeout=10.0)
        if not window:
            self.log(f"terminal {title!r} never appeared")
            return None
        window_id = int(window["id"])
        try:
            self.niri("action", "move-window-to-floating", "--id", str(window_id))
        except subprocess.CalledProcessError:
            pass
        self.place(window_id, x, y, width, height)
        return window_id

    def close_terminals(self) -> None:
        for window in self.windows():
            if window.get("app_id") == "Alacritty":
                self.close_window(int(window["id"]))

    # ---------------------------------------------------------- storyboard
    def phase_intro(self) -> None:
        self.title("lambdock", "a dock you can reprogram while it runs")
        self.park()
        time.sleep(2.3)
        self.title_end()

    def phase_autohide(self) -> None:
        self.caption("Auto-hide: the dock waits at the screen edge")
        # An instant jump to the reveal strip is important: a stream of motion
        # events while the dock slides in can make it hide again.
        self.move(self.out_w / 2, self.out_h - 1)
        time.sleep(0.9)
        # Step onto the dock so it does not immediately hide again.
        self.move(self.X(ITEM_ALACRITTY), self.Y(DOCK_ROW))
        time.sleep(0.6)
        self.glide(self.out_w / 2, self.out_h - 300, 700)
        time.sleep(1.2)
        self.caption_end()

        # Hot-reload the settings file, which also showcases the file watcher:
        # the dock comes back and stops auto-hiding for the rest of the demo.
        # The reload keeps the dock where it is (off screen), so the reveal
        # strip is touched again to bring it back.
        settings = Path(self.args.settings)
        text = settings.read_text(encoding="utf-8")
        text = text.replace("(define dock-auto-hide? #t)", "(define dock-auto-hide? #f)")
        self.write_atomic(settings, text)
        self.move(self.out_w / 2, self.out_h - 1)
        time.sleep(0.9)
        self.move(self.X(ITEM_ALACRITTY), self.Y(DOCK_ROW))
        time.sleep(0.8)

    def phase_magnify(self) -> None:
        self.caption("Fish-eye magnification, right under the pointer")
        # Stay inside the capsule for the whole sweep: the dock tucks itself
        # away when the pointer leaves it, and a sweep that starts outside would
        # hide the very thing it is demonstrating.  Stopping before the clock
        # also keeps the row from outgrowing its capsule.
        self.move(self.X(238), self.Y(DOCK_ROW))
        time.sleep(0.4)
        self.glide(self.X(700), self.Y(DOCK_ROW), 1900)
        time.sleep(0.2)
        self.glide(self.X(320), self.Y(DOCK_ROW), 1000)
        time.sleep(0.15)
        self.caption_end()

    def phase_launch(self) -> None:
        self.caption("Click to launch; running apps get an indicator")
        self.glide(self.X(ITEM_ALACRITTY), self.Y(DOCK_ROW), 450)
        self.click()
        self.spawn_terminal("work", "fetch", 66, 118, 500, 330)
        time.sleep(0.45)
        self.spawn_terminal("files", "files", 562, 118, 500, 330)
        time.sleep(1.3)
        self.caption_end()

    def phase_popover(self) -> None:
        self.caption("Right-click for the windows of a running app")
        self.glide(self.X(ITEM_ALACRITTY), self.Y(DOCK_ROW), 500)
        self.right_click()
        time.sleep(0.9)
        # Glide up through the window rows, then press the last one: Close All.
        for y in (520, 560, 600):
            self.glide(self.X(ITEM_ALACRITTY), self.Y(y), 340)
            time.sleep(0.15)
        self.glide(self.X(ITEM_ALACRITTY), self.Y(POPOVER_CLOSE_ALL), 380)
        time.sleep(0.4)
        self.click()
        time.sleep(1.2)
        self.move(self.out_w / 2, self.out_h * 0.4)
        self.caption_end()

    def phase_preferences(self) -> None:
        self.caption("Right-click the dock for its menu; every setting is live")
        # Clear the stage: the dialog is the subject of this scene, and a
        # floating terminal behind it reads as clutter.  Close the windows, then
        # dismiss any popover they left latched open -- a popover grabs input and
        # would swallow the right click below.
        self.close_terminals()
        time.sleep(0.5)
        self.move(self.out_w / 2, self.out_h * 0.3)
        self.click()
        time.sleep(0.3)

        # Freeze the fish-eye animation so the separator stays at a known
        # coordinate while the menu is opened.
        self.dock_eval("(begin (set-dock-hover-animate! #f) (reload-dock!))")
        time.sleep(0.5)
        self.reveal_dock(self.X(ITEM_SEPARATOR))
        self.move(self.X(ITEM_SEPARATOR), self.Y(MONTAGE_ROW))

        before = {int(window["id"]) for window in self.windows()}
        self.move(self.X(ITEM_SEPARATOR), self.Y(DOCK_ROW))
        self.right_click()
        time.sleep(0.8)
        self.glide(self.X(ITEM_SEPARATOR), self.Y(624), 380)
        time.sleep(0.2)
        self.click()
        time.sleep(1.6)

        dialog = self.wait_window(title="Lambdock Preferences", timeout=6.0)
        if dialog is None:
            dialog = next(
                (window for window in self.windows() if int(window["id"]) not in before),
                None,
            )

        if dialog is not None:
            window_id = int(dialog["id"])
            # The dialog is tiled unless it is floated, and a tiled window gives
            # up its own size, so floating it first is what puts it above the
            # dock at all.  Its own size request then lands late -- one
            # placement is undone by it -- so the placement is applied twice
            # with a gap, low enough that the caption pill above it does not sit
            # on the title bar and short enough to leave the dock in view.
            self.niri_or_log("move-window-to-floating", "--id", str(window_id))
            self.niri_or_log("focus-window", "--id", str(window_id))
            for _ in range(2):
                self.place(window_id, 250, 122, 620, 548)
                time.sleep(0.6)
            time.sleep(1.8)
            self.close_window(window_id)
        time.sleep(0.5)
        self.dock_eval("(begin (set-dock-hover-animate! #t) (reload-dock!))")
        time.sleep(0.3)
        self.caption_end()

    def niri_or_log(self, *action: str) -> None:
        try:
            self.niri("action", *action)
        except subprocess.CalledProcessError as error:
            self.log(f"{action[0]} failed: {error}")

    def phase_stack(self) -> None:
        self.caption("A folder becomes a stack you can browse")
        self.glide(self.X(ITEM_STACK), self.Y(DOCK_ROW), 550)
        self.click()
        time.sleep(1.0)
        for x, y in ((600, 420), (660, 420), (600, 500)):
            self.glide(self.X(x), self.Y(y), 320)
            time.sleep(0.15)
        # Dismiss the popover by clicking the empty top of the screen: a click
        # inside it would launch a file.
        self.move(60, 60)
        self.click()
        time.sleep(0.7)
        self.caption_end()

    def phase_launchpad(self) -> None:
        self.caption("The launchpad: type to filter it, or drive it from Scheme")
        self.dock_eval("(show-launchpad!)")
        time.sleep(1.5)
        self.dock_eval('(launchpad-search! "term")')
        time.sleep(1.3)
        self.dock_eval('(launchpad-search! "calc")')
        time.sleep(1.3)
        self.dock_eval('(launchpad-search! "")')
        time.sleep(0.7)
        self.dock_eval("(hide-launchpad!)")
        time.sleep(1.0)
        self.caption_end()

    def montage_theme(self, theme: str) -> None:
        """Swap one theme through the REPL, with the shelf for Mavericks."""
        shelf = theme.startswith("macos-mavericks")
        call = f"(swap! '{theme} 'shelf)" if shelf else f"(swap! '{theme})"
        # The Mavericks slab slides in and mirrors the icons onto its glass, so
        # it needs a beat longer on screen than a straight colour change.
        self.repl(call, speed_ms=8, hold=1.5 if shelf else 0.4)
        self.theme(theme)

    def phase_repl(self) -> None:
        self.caption("Reprogram the running dock over its socket REPL")
        kitty_id = self.launch_repl_window()
        self.open_repl_writer()
        # Park the pointer on the dock (that is what keeps it on screen) and
        # freeze the fish-eye, so the icon under it is not magnified -- a
        # magnified row outgrows its capsule, and the montage is about the
        # capsule.
        self.dock_eval("(begin (set-dock-hover-animate! #f) (reload-dock!))")
        time.sleep(0.4)
        self.reveal_dock(self.X(ITEM_SEPARATOR))
        self.move(self.X(ITEM_SEPARATOR), self.Y(MONTAGE_ROW))

        self.repl("(use-modules (lambdock core))", speed_ms=10, hold=0.4)
        self.repl(SWAP_DEFINITION, speed_ms=6, hold=0.6)
        self.repl("(get-dock-icon-size)", speed_ms=14, hold=0.7)
        self.repl("(set-dock-icon-size! 64) (reload-dock!)", speed_ms=10, hold=0.9)
        self.repl("(set-dock-icon-size! 48) (reload-dock!)", speed_ms=10, hold=0.6)
        self.caption_end()

        self.caption("Built-in themes, swapped live")
        for theme in DARK_THEMES:
            self.montage_theme(theme)
        self.switch_wallpaper("mountains.png")
        for theme in LIGHT_THEMES:
            self.montage_theme(theme)
        # The montage stops on a theme, so let the last one stand before its
        # label goes away: that frame is the only one showing the light
        # Mavericks slab, and its mirrored icons are the whole point of it.
        time.sleep(1.5)
        self.theme_end()
        self.caption_end()
        # Leave the shelf and the frozen fish-eye behind.
        self.dock_eval(f"(begin {SHELF_OFF} (reload-dock!))")
        self.dock_eval("(begin (set-dock-hover-animate! #t) (reload-dock!))")
        # Step off the capsule: a pointer resting on it leaves a tooltip behind
        # for the next scene.
        self.move(self.out_w / 2, self.out_h * 0.45)
        time.sleep(0.3)

        if self.repl_fd is not None:
            os.close(self.repl_fd)
            self.repl_fd = None
        self.close_window(kitty_id)
        time.sleep(0.7)

    def phase_taskbar(self) -> None:
        # The windows come first: the caption should cover the moment the bar
        # changes, not the second and a half it takes to open two terminals.
        self.spawn_terminal("build", "fetch", 66, 118, 500, 330)
        self.spawn_terminal("logs", "files", 562, 118, 500, 330)
        time.sleep(0.6)
        self.caption("Or a Windows XP-style taskbar")
        self.reveal_dock(self.X(ITEM_SEPARATOR))
        # Back off the bar before it can grow a tooltip under the pointer.
        self.move(self.out_w / 2, self.out_h * 0.45)

        # The mode change re-anchors the layer surface, which a layout reload
        # only picks up on the next pass: nudge it twice so the bar really is
        # full width.
        self.dock_eval(f"(begin {SHELF_OFF} (set-dock-theme! 'windows-xp) (reload-dock!))")
        time.sleep(0.6)
        self.dock_eval("(begin (set-dock-mode! 'taskbar) (reload-dock!))")
        time.sleep(1.0)
        self.dock_eval("(begin (set-dock-mode! 'taskbar) (reload-dock!))")
        time.sleep(2.0)
        self.caption_end()

    def phase_outro(self) -> None:
        # Back to the default look *before* the card goes up, so the closing
        # frame is the same dock the README's hero image shows.
        self.close_terminals()
        time.sleep(0.3)
        self.dock_eval("(begin (set-dock-mode! 'dock) (reload-dock!))")
        time.sleep(0.5)
        self.dock_eval(f"(begin {SHELF_OFF} (set-dock-theme! 'vanilla) (reload-dock!))")
        self.switch_wallpaper("galaxy.png")
        time.sleep(0.5)
        self.move(self.out_w / 2, self.out_h * 0.45)
        time.sleep(0.3)

        self.title("lambdock", "github.com/jjba23/lambdock")
        time.sleep(2.8)
        self.title_end()

    # ---------------------------------------------------------- main
    def run(self) -> None:
        self.events_file = open(self.events_path, "w", encoding="utf-8")
        self.write_atomic(self.tmp / "nested-ready", "1")
        self.setup()
        self.wait_for(self.tmp / "go", timeout=60.0)
        self.t0 = time.monotonic()
        self.log("storyboard started")
        # The recorder is already capturing: give it a beat of quiet so the
        # fade-in lands on a still desktop.
        time.sleep(0.35)

        self.phase_intro()
        self.phase_autohide()
        self.phase_magnify()
        self.phase_launch()
        self.phase_popover()
        self.phase_preferences()
        self.phase_stack()
        self.phase_launchpad()
        self.phase_repl()
        self.phase_taskbar()
        self.phase_outro()

        self.log(f"storyboard finished at {time.monotonic() - self.t0:.2f}s")
        self.write_atomic(self.tmp / "done", "1")

    def cleanup(self) -> None:
        if self.vptr is not None:
            try:
                self.vp("quit")
                self.vptr.wait(timeout=2.0)
            except Exception:
                try:
                    self.vptr.kill()
                except Exception:
                    pass
        if self.events_file is not None:
            self.events_file.close()
        if self.repl_fd is not None:
            try:
                os.close(self.repl_fd)
            except OSError:
                pass


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tmp", required=True)
    parser.add_argument("--root", required=True)
    parser.add_argument("--lambdock", required=True)
    parser.add_argument("--settings", required=True)
    parser.add_argument("--repl-socket", required=True)
    parser.add_argument("--repl-fifo", required=True)
    parser.add_argument("--repl-done", required=True)
    parser.add_argument("--repl-ready", required=True)
    args = parser.parse_args()

    demo = Demo(args)
    try:
        demo.run()
    except BaseException:
        traceback.print_exc()
        with open(Path(args.tmp) / "controller.log", "a", encoding="utf-8") as handle:
            handle.write(traceback.format_exc())
        try:
            Path(args.tmp, "error").write_text(traceback.format_exc(), encoding="utf-8")
        except OSError:
            pass
        return 1
    finally:
        demo.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
