#!/usr/bin/env python3
# lambdock -- hyper-hackable desktop dock for Wayland
#
# SPDX-License-Identifier: MIT
# Copyright © Josep Bigorra <jjbigorra@gmail.com>

"""Measure where the dock's items sit in a recording, in nested logical pixels.

    measure_dock.py --raw raw.mkv [--time 12] [--edge 56] [--phys-width 2256]

The storyboard addresses dock items by coordinate, so the numbers in
controller.py have to match what the dock actually draws.  This tool takes one
frame of a recording, finds the capsule and the bright icon runs inside it, and
prints the item centres in the same coordinate space the controller uses --
plus a ready-to-paste table.

Run it whenever the generated settings file changes (another item, another icon
size, another padding): adding or removing a slot moves every item.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image


def runs(mask: np.ndarray, minimum: int) -> list[tuple[int, int]]:
    found: list[tuple[int, int]] = []
    start = None
    for index, value in enumerate(mask):
        if value and start is None:
            start = index
        elif not value and start is not None:
            found.append((start, index - 1))
            start = None
    if start is not None:
        found.append((start, len(mask) - 1))
    return [(a, b) for a, b in found if b - a >= minimum]


def longest_run(mask: np.ndarray, minimum: int = 20) -> tuple[int, int]:
    candidates = runs(mask, minimum)
    return max(candidates, key=lambda r: r[1] - r[0]) if candidates else (0, 0)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--raw", required=True, help="lossless recording")
    parser.add_argument("--time", type=float, default=12.0, help="frame to measure")
    parser.add_argument("--edge", type=int, default=56, help="crop margin used")
    parser.add_argument("--phys-width", type=int, default=2256, help="output width")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as tmpdir:
        frame_path = Path(tmpdir) / "frame.png"
        subprocess.run(
            [
                "ffmpeg", "-y", "-loglevel", "error", "-ss", f"{args.time}",
                "-i", args.raw, "-frames:v", "1", str(frame_path),
            ],
            check=True,
        )
        frame = np.asarray(Image.open(frame_path).convert("RGB"), np.float32) / 255.0

    height, width = frame.shape[:2]
    scale = width / (args.phys_width - 2 * args.edge)  # capture px per physical px

    def logical(raw_x: float) -> float:
        return args.edge / 2.0 + raw_x / scale / 2.0

    band = frame[int(height * 0.90):, :, :]
    bright = (band.max(axis=2) > 0.45).mean(axis=0)
    items = runs(bright > 0.08, 6)

    print(f"frame {width}x{height}: capsule and item centres in nested logical px")
    gradient = np.abs(np.diff(band.mean(axis=2), axis=0))
    row = int(np.argmax((gradient > 0.012).sum(axis=1)))
    left, right = longest_run(gradient[row] > 0.012)
    print(f"  capsule  {logical(left):7.1f} .. {logical(right):7.1f}"
          f"   centre {logical((left + right) / 2):7.1f}")
    for start, end in items:
        centre = logical((start + end) / 2.0)
        print(f"  item     {logical(start):7.1f} .. {logical(end):7.1f}"
              f"   centre {centre:7.1f}")
    print()
    print("The dock is centred on the output when the two capsule numbers above")
    print("add up to the output width; if they do not, the crop is off-centre.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
