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
sleeps and coordinates.  Every phase is timestamped in events.tsv so the outer
script can burn matching captions into the final GIF.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import signal
import socket
import subprocess
import sys
import time
import traceback
from pathlib import Path

# The storyboard coordinates below were designed on a 1128x752 logical output
# (2256x1504 physical at scale 2).  The dock is anchored to the bottom edge and
# centered horizontally, so a different output size only needs a translation.
REF_W = 1128
REF_H = 752

# Friendly names for the theme overlay burnt into the GIF.
THEME_LABELS = {
    "vanilla": "Vanilla Glassmorphism",
    "macos-big-sur": "macOS Big Sur",
    "macos-ventura": "macOS Ventura",
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
    "vanilla",
    "macos-big-sur",
    "macos-ventura",
    "unity",
    "nature",
    "snow-leopard-dark",
    "windows-10",
    "windows-11",
]
LIGHT_THEMES = [
    "vanilla-light",
    "snow-leopard",
    "solarized-light",
    "windows-xp",
    "none",
]


class Demo:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.tmp = Path(args.tmp)
        self.events_path = self.tmp / "events.tsv"
        self.log_path = self.tmp / "controller.log"
        self.events_file = None
        self.t0 = 0.0
        self.proc = None
        self.vptr = None
        self.repl_fd: int | None = None
        self.repl_index = 0
        self.repl_done = self.tmp / "repl.done"
        self.wallpaper_pid_file = self.tmp / "swaybg.pid"
        self.pointer_x = 0.0
        self.pointer_y = 0.0

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

    def close_window(self, window_id: int) -> None:
        try:
            self.niri("action", "close-window", "--id", str(window_id))
        except subprocess.CalledProcessError:
            pass

    # ---------------------------------------------------------- coordinates
    @property
    def width(self) -> int:
        return self.out_w

    @property
    def height(self) -> int:
        return self.out_h

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

    # ---------------------------------------------------------- vptr driver
    def start_vptr(self) -> None:
        """Drive the *outer* compositor's pointer.

        Motion events from a virtual pointer are not enough for layer-shell
        enter/leave and fish-eye effects in a nested niri, but the outer
        compositor forwards its pointer as a normal pointer to the nested
        window.  So we move the outer pointer and translate nested coordinates
        to outer logical coordinates.
        """
        self.outer_display = os.environ.get("OUTER_DISPLAY", os.environ.get("WAYLAND_DISPLAY", ""))
        self.outer_w = int(os.environ.get("OUTER_W", self.out_w))
        self.outer_h = int(os.environ.get("OUTER_H", self.out_h))
        self.outer_scale = float(os.environ.get("OUTER_SCALE", "1"))
        self.nested_scale = float(os.environ.get("NESTED_SCALE", "1"))
        self.log(
            f"outer pointer {self.outer_w}x{self.outer_h} display={self.outer_display!r} "
            f"(outer scale {self.outer_scale}, nested scale {self.nested_scale})"
        )
        env = dict(os.environ)
        env["WAYLAND_DISPLAY"] = self.outer_display
        self.vptr = subprocess.Popen(
            [str(self.tmp / "vptr" / "vptr"), str(self.outer_w), str(self.outer_h)],
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
        self.log(f"virtual pointer ready on {self.out_w}x{self.out_h}")

    def vp(self, command: str) -> None:
        assert self.vptr is not None and self.vptr.stdin is not None
        elapsed = time.monotonic() - self.t0 if self.t0 else 0.0
        self.log(f"vp {elapsed:6.2f} {command}")
        self.vptr.stdin.write(command + "\n")
        self.vptr.stdin.flush()

    def to_outer(self, x: float, y: float) -> tuple[float, float]:
        factor = self.nested_scale / self.outer_scale
        return x * factor, y * factor

    def normalize(self, x: float, y: float) -> tuple[float, float]:
        """Nested logical pointer coordinates -> cropped GIF fractions."""
        edge = float(os.environ.get("DEMO_EDGE", "0"))
        phys_w = float(os.environ.get("DEMO_PHYS_W", "1"))
        phys_h = float(os.environ.get("DEMO_PHYS_H", "1"))
        nx = (x * self.nested_scale - edge) / (phys_w - edge)
        ny = (y * self.nested_scale - edge) / (phys_h - edge)
        return nx, ny

    def pointer_event(self, kind: str, text: str) -> None:
        if self.t0:
            self.event(kind, text)

    def park(self) -> None:
        """Leave the pointer in the corner removed by the GIF crop."""
        self.vp("move 5 5")
        self.pointer_x = 0.0
        self.pointer_y = 0.0
        time.sleep(0.06)

    def move(self, x: float, y: float) -> None:
        ox, oy = self.to_outer(x, y)
        nx, ny = self.normalize(x, y)
        self.pointer_event("pointer_move", f"{nx:.5f},{ny:.5f}")
        self.vp(f"move {ox:.0f} {oy:.0f}")
        self.pointer_x, self.pointer_y = x, y
        time.sleep(0.06)

    def glide(self, x: float, y: float, ms: int) -> None:
        ox, oy = self.to_outer(x, y)
        sx, sy = self.normalize(self.pointer_x, self.pointer_y)
        ex, ey = self.normalize(x, y)
        self.pointer_event("pointer_glide", f"{sx:.5f},{sy:.5f},{ex:.5f},{ey:.5f},{ms}")
        self.vp(f"glide {ox:.0f} {oy:.0f} {ms}")
        self.pointer_x, self.pointer_y = x, y
        time.sleep(ms / 1000.0 + 0.04)

    def click(self) -> None:
        nx, ny = self.normalize(self.pointer_x, self.pointer_y)
        self.pointer_event("pointer_click", f"{nx:.5f},{ny:.5f}")
        self.vp("click")
        time.sleep(0.12)

    def right_click(self) -> None:
        nx, ny = self.normalize(self.pointer_x, self.pointer_y)
        self.pointer_event("pointer_click", f"{nx:.5f},{ny:.5f}")
        self.vp("rightclick")
        time.sleep(0.12)

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

        end = time.monotonic() + 15.0
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
        if os.environ.get("DEMO_PAUSE_AFTER_READY"):
            self.write_atomic(self.tmp / "paused", "1")
            while not (self.tmp / "resume").exists():
                time.sleep(0.1)
            self.log("resumed after debug pause")

    def start_swaybg(self, image_name: str) -> None:
        image = self.tmp / "assets" / image_name
        pid_file = self.wallpaper_pid_file
        script = (
            f"echo $$ > {shlex.quote(str(pid_file))}; "
            f"exec swaybg -i {shlex.quote(str(image))} -m fill"
        )
        self.spawn_sh(script)
        end = time.monotonic() + 5.0
        while time.monotonic() < end:
            if pid_file.exists():
                time.sleep(0.25)
                return
            time.sleep(0.05)
        raise RuntimeError("swaybg did not start")

    def switch_wallpaper(self, image_name: str) -> None:
        try:
            pid = int(self.wallpaper_pid_file.read_text().strip())
            os.kill(pid, signal.SIGTERM)
        except (OSError, ValueError):
            pass
        try:
            self.wallpaper_pid_file.unlink()
        except FileNotFoundError:
            pass
        self.start_swaybg(image_name)
        time.sleep(0.35)

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

    def launch_kitty(self) -> int:
        repl_ui = Path(self.args.root) / "tools" / "record-demo" / "repl_ui.py"
        command = (
            "exec kitty --title "
            + shlex.quote("lambdock REPL")
            + " -o font_size=16 -o background_opacity=0.90"
            + " -o hide_window_decorations=yes"
            + " -o window_padding_width=18 python3 "
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

        window = None
        end = time.monotonic() + 15.0
        while time.monotonic() < end:
            window = self.find_window(title="lambdock REPL")
            if window:
                break
            time.sleep(0.1)
        if not window:
            raise RuntimeError("kitty did not open")

        window_id = int(window["id"])
        try:
            self.niri("action", "move-window-to-floating", "--id", str(window_id))
        except subprocess.CalledProcessError:
            pass
        self.niri("action", "set-window-width", "--id", str(window_id), "1000")
        self.niri("action", "set-window-height", "--id", str(window_id), "500")
        self.niri("action", "center-window", "--id", str(window_id))
        self.niri("action", "focus-window", "--id", str(window_id))
        self.wait_for(Path(self.args.repl_ready), timeout=15.0)
        time.sleep(0.4)
        self.log(f"REPL terminal ready (window {window_id})")
        return window_id

    def open_repl_writer(self) -> None:
        # Opening the FIFO blocks until repl_ui opens its read end.
        self.repl_fd = os.open(self.args.repl_fifo, os.O_WRONLY)

    def repl(self, expression: str, speed_ms: int = 10, hold: float = 0.0) -> None:
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
            # One slow reload is not worth throwing away a five-minute
            # recording: log it and let the next expression resync.
            self.log(f"REPL command timed out (continuing): {expression}")
        if hold:
            time.sleep(hold)

    # ---------------------------------------------------------- storyboard
    def phase_autohide(self) -> None:
        self.caption("Auto-hide -- the dock slides away and returns at the screen edge")
        # An instant jump to the reveal strip is important: a stream of motion
        # events while the dock slides in can make it hide itself again.
        self.move(self.out_w / 2, self.out_h - 2)
        time.sleep(0.5)
        # Move into the revealed dock so it does not immediately hide again.
        self.move(self.out_w / 2, self.Y(704))
        time.sleep(1.2)
        self.glide(self.out_w / 2, self.out_h - 220, 800)
        time.sleep(1.4)
        self.caption_end()

        # Hot-reload the settings file, which also showcases the file watcher:
        # the dock comes back and stops auto-hiding for the rest of the demo.
        settings = Path(self.args.settings)
        text = settings.read_text(encoding="utf-8")
        text = text.replace("(define dock-auto-hide? #t)", "(define dock-auto-hide? #f)")
        self.write_atomic(settings, text)
        self.move(self.out_w / 2, self.out_h - 80)
        time.sleep(1.4)

    def phase_magnify(self) -> None:
        self.caption("Native fish-eye magnification under the pointer")
        self.move(self.X(180), self.Y(704))
        time.sleep(0.35)
        self.glide(self.X(940), self.Y(704), 2200)
        time.sleep(0.25)
        self.glide(self.X(180), self.Y(704), 1800)
        time.sleep(0.25)
        self.caption_end()

    def phase_launch(self) -> None:
        self.caption("Launch applications; running apps get an indicator")
        self.glide(self.X(404), self.Y(704), 650)
        self.click()
        time.sleep(1.8)

        calculator = self.find_window(app_id="org.gnome.Calculator")
        if calculator:
            self.close_window(int(calculator["id"]))
        time.sleep(0.5)

        for title in ("Alpha workspace", "Beta notes", "Gamma logs"):
            self.spawn_sh(
                "exec alacritty --title "
                + shlex.quote(title)
                + " -o window.decorations=none"
            )
            time.sleep(0.8)
        time.sleep(0.6)
        self.caption_end()

    def phase_popover(self) -> None:
        self.caption("Window popover HUD: focus a window, open another, close them")
        self.glide(self.X(199), self.Y(704), 600)
        self.right_click()
        time.sleep(0.8)

        for y in (456, 509, 554):
            self.glide(self.X(199), self.Y(y), 380)
            time.sleep(0.12)
        self.glide(self.X(199), self.Y(509), 300)
        self.click()
        time.sleep(0.8)

        self.glide(self.X(199), self.Y(704), 350)
        self.right_click()
        time.sleep(0.7)
        self.glide(self.X(199), self.Y(609), 420)
        self.click()
        time.sleep(0.9)
        # If New Instance left the popover open, Close All dismisses it and
        # demonstrates the last row of the popover.
        self.glide(self.X(199), self.Y(656), 350)
        self.click()
        time.sleep(1.0)
        self.move(self.out_w / 2, self.out_h * 0.35)
        self.click()
        time.sleep(0.4)
        self.caption_end()

    def phase_menu_about(self) -> None:
        self.caption("Right-click the dock for its own menu and About lambdock")
        # Make sure no popover is latched open before opening the menu.
        self.move(self.out_w / 2, self.out_h * 0.35)
        self.click()
        time.sleep(0.3)

        # Freeze the fish-eye animation so the separator stays at a known
        # coordinate while the menu is opened.
        self.dock_eval("(begin (set-dock-hover-animate! #f) (reload-dock!))")
        time.sleep(0.4)

        before = {int(window["id"]) for window in self.windows()}
        self.move(self.X(465), self.Y(704))
        self.right_click()
        time.sleep(0.7)
        self.move(self.X(465), self.Y(653))
        self.click()
        time.sleep(1.2)
        time.sleep(1.6)

        dialog = None
        for _ in range(30):
            dialog = next(
                (window for window in self.windows() if int(window["id"]) not in before),
                None,
            )
            if dialog:
                break
            time.sleep(0.1)
        if dialog is not None:
            self.close_window(int(dialog["id"]))
        else:
            try:
                self.niri("action", "close-window")
            except subprocess.CalledProcessError:
                pass
        time.sleep(0.6)
        self.dock_eval("(begin (set-dock-hover-animate! #t) (reload-dock!))")
        time.sleep(0.3)
        self.caption_end()

        # The popover demo closed every Alacritty window.  Open a few fresh
        # ones for the taskbar-with-titles part of the REPL segment.
        for title in ("Alpha workspace", "Beta notes", "Gamma logs"):
            self.spawn_sh(
                "exec alacritty --title "
                + shlex.quote(title)
                + " -o window.decorations=none"
            )
            time.sleep(0.7)

    def phase_stack(self) -> None:
        self.caption("Folder stacks: a whole folder behind one icon")
        # The stack sits where the btop preset used to be: replacing one
        # icon-only item with another keeps every other coordinate valid.
        self.glide(self.X(548), self.Y(704), 600)
        self.click()
        time.sleep(1.1)

        # Browse the grid, then dismiss it by clicking well outside: the popover
        # is tall, so a click in the middle of the screen would land inside it.
        for x, y in ((580, 545), (650, 545), (580, 615)):
            self.glide(self.X(x), self.Y(y), 340)
            time.sleep(0.18)
        self.move(40, 40)
        self.click()
        time.sleep(0.6)
        self.caption_end()

    def phase_launchpad(self) -> None:
        self.caption("The launchpad: search it by typing -- or from the REPL")
        # The launchpad is a C widget, so the dock exposes it to Scheme
        # (show-launchpad! / launchpad-search! / hide-launchpad!). Driving it
        # this way also shows the point of the socket: the dock is scriptable.
        self.dock_eval("(show-launchpad!)")
        time.sleep(1.4)
        self.dock_eval('(launchpad-search! "term")')
        time.sleep(1.2)
        self.dock_eval('(launchpad-search! "calc")')
        time.sleep(1.2)
        self.dock_eval("(launchpad-search! \"\")")
        time.sleep(0.8)
        self.dock_eval("(hide-launchpad!)")
        time.sleep(0.9)
        self.caption_end()

    def phase_preferences(self) -> None:
        self.caption("Preferences: every setting, applied to the running dock")
        before = {int(window["id"]) for window in self.windows()}

        # Clear the stage: a floating terminal would sit on top of the dialog.
        # They come back below, for the taskbar segment.
        for window in self.windows():
            if window.get("app_id") == "Alacritty":
                self.close_window(int(window["id"]))
        time.sleep(0.6)

        # Dismiss anything a previous scene left latched open: a popover that
        # grabs input would swallow the right click below.
        self.move(40, 40)
        self.click()
        time.sleep(0.4)

        # Same anchor as the About scene (the menu opens above the click point),
        # with Preferences as its first row.
        self.move(self.X(465), self.Y(704))
        self.right_click()
        time.sleep(0.8)
        self.move(self.X(465), self.Y(620))
        self.click()
        time.sleep(1.8)

        dialog = None
        for _ in range(30):
            dialog = next(
                (window for window in self.windows() if int(window["id"]) not in before),
                None,
            )
            if dialog:
                break
            time.sleep(0.1)

        if dialog is not None:
            window_id = int(dialog["id"])
            for action in (
                ("focus-window", "--id", str(window_id)),
                ("set-window-width", "--id", str(window_id), "860"),
                ("set-window-height", "--id", str(window_id), "760"),
                ("center-window", "--id", str(window_id)),
            ):
                try:
                    self.niri("action", *action)
                except subprocess.CalledProcessError:
                    pass
            time.sleep(2.4)
            self.close_window(window_id)
        time.sleep(0.5)
        self.caption_end()

        # The taskbar segment wants titled windows again.
        for title in ("Alpha workspace", "Beta notes", "Gamma logs"):
            self.spawn_sh(
                "exec alacritty --title "
                + shlex.quote(title)
                + " -o window.decorations=none"
            )
            time.sleep(0.7)

    def phase_repl(self) -> None:
        self.caption("Live socket REPL: mutate the running dock, no restart")
        kitty_id = self.launch_kitty()
        self.open_repl_writer()

        self.repl("(use-modules (lambdock core))", speed_ms=10, hold=0.45)
        self.repl("(get-dock-theme)", speed_ms=18, hold=0.5)
        self.repl("(get-dock-icon-size)", speed_ms=18, hold=0.5)
        for size, hold in ((56, 0.55), (64, 0.65), (48, 0.5)):
            self.repl(
                f"(begin (set-dock-icon-size! {size}) (reload-dock!))",
                speed_ms=5,
                hold=hold,
            )

        self.caption_end()
        self.caption("Hot-swapping every built-in theme, live")
        for theme in DARK_THEMES:
            self.repl(
                f"(begin (set-dock-theme! '{theme}) (reload-dock!))",
                speed_ms=4,
                hold=0.34,
            )
            self.theme(theme)

        self.switch_wallpaper("mountains.png")
        for theme in LIGHT_THEMES:
            self.repl(
                f"(begin (set-dock-theme! '{theme}) (reload-dock!))",
                speed_ms=4,
                hold=0.34,
            )
            self.theme(theme)
        self.theme_end()
        self.caption_end()

        self.caption("Taskbar mode with window titles")
        self.repl(
            "(begin (set-dock-theme! 'windows-xp) (reload-dock!))",
            speed_ms=5,
            hold=0.5,
        )
        self.repl(
            "(begin (set-dock-mode! 'taskbar) (reload-dock!))",
            speed_ms=5,
            hold=1.0,
        )
        self.repl(
            "(begin (set-dock-show-window-titles! #t) (reload-dock!))",
            speed_ms=5,
            hold=1.5,
        )
        self.caption_end()

        self.repl(
            "(begin (set-dock-show-window-titles! #f) (reload-dock!))",
            speed_ms=5,
            hold=0.35,
        )
        self.repl(
            "(begin (set-dock-mode! 'dock) (reload-dock!))",
            speed_ms=5,
            hold=0.8,
        )
        self.repl(
            "(begin (set-dock-theme! 'vanilla-light) (reload-dock!))",
            speed_ms=5,
            hold=0.7,
        )

        if self.repl_fd is not None:
            os.close(self.repl_fd)
            self.repl_fd = None
        self.close_window(kitty_id)
        time.sleep(0.6)

    def phase_cleanup(self) -> None:
        # Close the extra terminals opened for the taskbar demo.  The popover's
        # Close All row was already demonstrated earlier.
        for window in self.windows():
            if window.get("app_id") == "Alacritty":
                self.close_window(int(window["id"]))
        time.sleep(0.7)
        self.move(self.out_w / 2, self.out_h * 0.35)
        time.sleep(0.4)

        self.caption("Config, CSS, state and window tracking -- all live")
        self.glide(self.X(180), self.Y(704), 900)
        self.glide(self.X(900), self.Y(704), 1500)
        self.move(self.out_w / 2, self.out_h * 0.35)
        self.caption_end()

        self.caption("lambdock -- a dock you can reprogram while it runs")
        time.sleep(2.0)
        self.caption_end()

    # ---------------------------------------------------------- main
    def run(self) -> None:
        self.events_file = open(self.events_path, "w", encoding="utf-8")
        self.write_atomic(self.tmp / "nested-ready", "1")
        self.setup()
        self.wait_for(self.tmp / "go", timeout=30.0)
        self.t0 = time.monotonic()
        self.log("storyboard started")
        # Give wf-recorder a moment to start capturing before the first action.
        time.sleep(0.6)

        self.phase_autohide()
        self.phase_magnify()
        self.phase_launch()
        self.phase_popover()
        self.phase_menu_about()
        self.phase_stack()
        self.phase_launchpad()
        self.phase_preferences()
        self.phase_repl()
        self.phase_cleanup()

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
