#!/usr/bin/env python3
# lambdock -- hyper-hackable desktop dock for Wayland
#
# SPDX-License-Identifier: MIT
# Copyright © Josep Bigorra <jjbigorra@gmail.com>

"""Generate the two demo wallpapers without any network access.

    make_wallpapers.py OUTDIR WIDTH HEIGHT

The galaxy sits behind the dark themes, the mountain haze behind the light
ones.  Everything is seeded, so two runs produce the same images.

Two rules keep these wallpapers usable behind a 256-colour GIF:

* no hard edges that survive a downscale badly -- the ridges are smooth curves
  rather than jagged polygons, which is what produced the moire the first
  version of this demo showed, and
* gradients plus a grain of about one 8-bit step, so the palette pass has
  something to dither instead of banding.

Requires numpy and Pillow (the recorder checks for both).
"""

from __future__ import annotations

import os
import sys

import numpy as np
from PIL import Image, ImageFilter


# ------------------------------------------------------------------ helpers
def hex_rgb(value: str) -> np.ndarray:
    value = value.lstrip("#")
    return np.array([int(value[i : i + 2], 16) / 255.0 for i in (0, 2, 4)], np.float32)


def vertical_gradient(height: int, stops: list[tuple[float, str]]) -> np.ndarray:
    """A smooth vertical ramp through colour stops given as (position, hex)."""
    positions = np.array([p for p, _ in stops], np.float32)
    colours = np.stack([hex_rgb(c) for _, c in stops])
    t = np.linspace(0.0, 1.0, height, dtype=np.float32)
    ramp = np.stack(
        [np.interp(t, positions, colours[:, channel]) for channel in range(3)], axis=1
    )
    return ramp[:, None, :]


def value_noise(width: int, height: int, resolution: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    small = (rng.random((resolution, resolution)) * 255).astype(np.uint8)
    image = Image.fromarray(small).resize((width, height), Image.BICUBIC)
    return np.asarray(image, np.float32) / 255.0


def fbm(width: int, height: int, seed: int, octaves: int = 6, base: int = 3) -> np.ndarray:
    total = np.zeros((height, width), np.float32)
    amplitude = 1.0
    norm = 0.0
    for octave in range(octaves):
        total += amplitude * value_noise(width, height, base * 2**octave, seed + octave)
        norm += amplitude
        amplitude *= 0.5
    return total / norm


def screen(base: np.ndarray, layer: np.ndarray) -> np.ndarray:
    return 1.0 - (1.0 - base) * (1.0 - layer)


def soft_glow(
    width: int, height: int, cx: float, cy: float, sigma: float, colour: str, strength: float
) -> np.ndarray:
    yy, xx = np.mgrid[0:height, 0:width]
    xn = xx.astype(np.float32) / width
    yn = yy.astype(np.float32) / height
    d2 = ((xn - cx) * (width / height)) ** 2 + (yn - cy) ** 2
    glow = np.exp(-d2 / (2.0 * sigma * sigma))[..., None] * strength
    return np.clip(glow, 0.0, 1.0) * hex_rgb(colour)


def save(array: np.ndarray, path: str, seed: int) -> None:
    # One 8-bit step of grain: enough to keep smooth ramps from banding after
    # the GIF's palette reduction, invisible at 100%.
    grain = (np.random.default_rng(seed).random(array.shape[:2] + (1,), np.float32) - 0.5)
    data = np.clip(array * 255.0 + grain, 0.0, 255.0).astype(np.uint8)
    Image.fromarray(data, "RGB").save(path, optimize=True)


# ------------------------------------------------------------------ galaxy
def make_galaxy(width: int, height: int, path: str) -> None:
    img = vertical_gradient(
        height,
        [
            (0.00, "#03040a"),
            (0.35, "#06080f"),
            (0.70, "#0a0d1c"),
            (1.00, "#04050b"),
        ],
    ) * np.ones((1, width, 1), np.float32)

    for cx, cy, sigma, colour, strength in [
        (0.30, 0.16, 0.42, "#1c2f6e", 0.50),
        (0.72, 0.30, 0.34, "#3b2a7d", 0.42),
        (0.16, 0.72, 0.36, "#0d4a63", 0.32),
        (0.86, 0.82, 0.30, "#4a2360", 0.26),
        (0.52, 0.52, 0.22, "#1e3a78", 0.20),
    ]:
        img = screen(img, soft_glow(width, height, cx, cy, sigma, colour, strength))

    yy, xx = np.mgrid[0:height, 0:width]
    xn = xx.astype(np.float32) / width
    yn = yy.astype(np.float32) / height

    # A diagonal nebula: one low-frequency band, warped by noise, then blurred
    # well past the pixel grid so nothing can alias.
    warp = (fbm(width, height, 11, octaves=5, base=2) - 0.5) * 0.09
    centre = 0.94 - 0.80 * xn + warp
    band = np.exp(-(((yn - centre) / 0.13) ** 2))
    texture = fbm(width, height, 23, octaves=6, base=3)
    band *= 0.30 + 0.90 * texture
    band = np.asarray(
        Image.fromarray((np.clip(band, 0, 1) * 255).astype(np.uint8)).filter(
            ImageFilter.GaussianBlur(max(2.0, height / 220.0))
        ),
        np.float32,
    ) / 255.0
    img = screen(img, band[..., None] * np.array([0.32, 0.30, 0.58], np.float32) * 0.62)

    # Vignette: pulls the eye to the dock and the captions, and keeps the frame
    # edges from competing with them.
    yy, xx = np.mgrid[0:height, 0:width]
    radius = np.sqrt(
        ((xx / width - 0.5) * 1.10) ** 2 + ((yy / height - 0.55) * 1.05) ** 2
    )
    vignette = np.clip(1.0 - 0.42 * radius**2, 0.55, 1.0)[..., None]
    img *= vignette

    # Stars: sparse, a couple of pixels across so they survive the downscale,
    # brightening where the nebula is dense.
    stars = np.zeros((height, width), np.float32)
    rng = np.random.default_rng(7)
    count = int(width * height / 3800)
    xs = rng.integers(0, width, count)
    ys = rng.integers(0, height, count)
    weights = rng.uniform(0.25, 1.0, count) ** 1.7
    for x, y, weight in zip(xs, ys, weights):
        stars[y, x] = max(stars[y, x], weight * (0.40 + 0.90 * band[y, x]))
    halo = np.asarray(
        Image.fromarray((np.clip(stars, 0, 1) * 255).astype(np.uint8)).filter(
            ImageFilter.GaussianBlur(1.6)
        ),
        np.float32,
    ) / 255.0
    img = screen(img, halo[..., None] * np.array([0.86, 0.92, 1.0], np.float32))
    img = screen(img, np.clip(stars, 0, 1)[..., None] * np.array([1.0, 1.0, 1.0], np.float32) * 0.55)

    save(np.clip(img, 0.0, 1.0), path, seed=5)


# ---------------------------------------------------------------- mountains
def ridge_line(width: int, height: int, base: float, amplitude: float, seed: int) -> np.ndarray:
    """Smooth, deterministic ridge height for every column."""
    rng = np.random.default_rng(seed)
    x = np.linspace(0.0, 1.0, width, dtype=np.float32)
    line = np.full_like(x, base)
    for harmonic in range(1, 5):
        phase = rng.uniform(0, 2 * np.pi)
        weight = 1.0 / harmonic
        line += amplitude * weight * np.sin(2 * np.pi * harmonic * x + phase)
    line += amplitude * 0.6 * (value_noise(width, 1, 64, seed + 7)[0] - 0.5)
    return line


def make_mountains(width: int, height: int, path: str) -> None:
    img = vertical_gradient(
        height,
        [
            (0.00, "#7c99bb"),
            (0.28, "#a9c1d6"),
            (0.48, "#e2e6e8"),
            (0.60, "#f6e4c8"),
            (1.00, "#3d4d63"),
        ],
    ) * np.ones((1, width, 1), np.float32)

    for cx, cy, sigma, colour, strength in [
        (0.68, 0.42, 0.30, "#f0cf9a", 0.30),
        (0.68, 0.42, 0.13, "#fff2d4", 0.85),
        (0.68, 0.42, 0.045, "#fffdf6", 1.0),
        (0.20, 0.22, 0.34, "#8fb0cd", 0.35),
    ]:
        img = screen(img, soft_glow(width, height, cx, cy, sigma, colour, strength))

    haze = np.exp(-(((np.linspace(0, 1, height, dtype=np.float32) - 0.72) / 0.10) ** 2))
    img = screen(img, haze[:, None, None] * hex_rgb("#f3ddc0") * 0.35)

    supersample = 2
    big = img.repeat(supersample, axis=0).repeat(supersample, axis=1)
    big_h, big_w = big.shape[:2]

    layers = [
        (0.575, 0.055, "#c3d2df", 0.55, 101),
        (0.665, 0.050, "#a6bacb", 0.45, 202),
        (0.755, 0.042, "#8098ae", 0.35, 303),
        (0.845, 0.032, "#5b7288", 0.25, 404),
        (0.920, 0.024, "#3b4d61", 0.18, 505),
    ]
    for base, amplitude, colour, haze_mix, seed in layers:
        line = ridge_line(big_w, big_h, base, amplitude, seed)
        rows = np.arange(big_h, dtype=np.float32)[:, None]
        ridge_top = (line * big_h)[None, :]
        coverage = np.clip((rows - ridge_top) / (big_h * 0.006), 0.0, 1.0)[..., None]
        fog = np.exp(-(((rows - ridge_top) / (big_h * 0.05)) ** 2))[..., None]
        layer_colour = (1.0 - haze_mix) * hex_rgb(colour) + haze_mix * hex_rgb("#e8eef2")
        big = big * (1.0 - coverage) + (layer_colour * (1.0 - fog * 0.35) + hex_rgb("#f4e8d6") * fog * 0.35) * coverage

    img = big.reshape(height, supersample, width, supersample, 3).mean(axis=(1, 3))
    save(np.clip(img, 0.0, 1.0), path, seed=9)


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    outdir, width, height = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    os.makedirs(outdir, exist_ok=True)
    make_galaxy(width, height, os.path.join(outdir, "galaxy.png"))
    make_mountains(width, height, os.path.join(outdir, "mountains.png"))
    print(f"wrote wallpapers to {outdir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
