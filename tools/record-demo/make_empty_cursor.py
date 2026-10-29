#!/usr/bin/env python3
# lambdock -- hyper-hackable desktop dock for Wayland
#
# SPDX-License-Identifier: MIT
# Copyright © Josep Bigorra <jjbigorra@gmail.com>

"""Write a fully transparent Xcursor theme.

    make_empty_cursor.py ICONDIR

The nested compositor still has to draw *a* cursor; this theme makes it draw
nothing.  That is how the recording ends up with exactly one cursor -- the
host's, which is the one that actually follows the virtual pointer -- instead of
two arrows a few pixels apart.

Everything is written under ICONDIR/cursors/, which is the layout libXcursor
looks for in ~/.local/share/icons/<theme>/.  No dependencies beyond the standard
library, and no network.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

MAGIC = 0x72756358  # "Xcur"
IMAGE_TYPE = 0xFFFD0002
# Every name a compositor may ask for; a missing name makes libXcursor fall back
# to its built-in arrow, which is exactly what we do not want.
NAMES = (
    "default", "left_ptr", "text", "pointer", "hand1", "hand2", "crosshair",
    "cross", "move", "fleur", "wait", "watch", "progress", "help", "context-menu",
    "n-resize", "s-resize", "e-resize", "w-resize", "ne-resize", "nw-resize",
    "se-resize", "sw-resize", "col-resize", "row-resize", "grab", "grabbing",
    "not-allowed", "zoom-in", "zoom-out", "top_left_corner", "top_right_corner",
    "bottom_left_corner", "bottom_right_corner",
)
SIZES = (24, 32, 48, 64)


def cursor_file(sizes: tuple[int, ...] = SIZES) -> bytes:
    """A cursor whose every image is 1x1... of fully transparent pixels."""
    chunks = []
    for size in sizes:
        header = struct.pack(
            "<IIIIIIIII",
            36,          # chunk header size
            IMAGE_TYPE,
            size,        # nominal size
            1,           # version
            size,        # width
            size,        # height
            0,           # xhot
            0,           # yhot
            1,           # delay (ms)
        )
        chunks.append(header + b"\x00\x00\x00\x00" * (size * size))

    header_size = 16
    toc_size = 12 * len(chunks)
    offset = header_size + toc_size
    positions = []
    for chunk in chunks:
        positions.append(offset)
        offset += len(chunk)

    out = bytearray()
    out += struct.pack("<IIII", MAGIC, header_size, 0x10000, len(chunks))
    for size, position in zip(sizes, positions):
        out += struct.pack("<III", IMAGE_TYPE, size, position)
    for chunk in chunks:
        out += chunk
    return bytes(out)


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    icons = Path(sys.argv[1])
    cursors = icons / "cursors"
    cursors.mkdir(parents=True, exist_ok=True)
    data = cursor_file()
    for name in NAMES:
        (cursors / name).write_bytes(data)
    print(f"wrote a transparent cursor theme to {icons}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
