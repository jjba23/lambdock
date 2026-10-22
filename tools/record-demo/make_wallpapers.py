#!/usr/bin/env python3
"""Generate the two demo wallpapers without any network access.

    make_wallpapers.py OUTDIR WIDTH HEIGHT

The galaxy is used behind the dark themes, the mountain sunrise behind the
light themes.  Everything is seeded, so two runs produce the same images.

Requires numpy and Pillow (the recorder checks for both).
"""

from __future__ import annotations

import os
import random
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter


def hex_rgb(value: str) -> np.ndarray:
    value = value.lstrip("#")
    return np.array([int(value[i : i + 2], 16) / 255.0 for i in (0, 2, 4)], np.float32)


def vertical_gradient(width: int, height: int, top: str, bottom: str) -> np.ndarray:
    t = np.linspace(0.0, 1.0, height, dtype=np.float32)[:, None, None]
    return hex_rgb(top) * (1.0 - t) + hex_rgb(bottom) * t


def screen(base: np.ndarray, layer: np.ndarray) -> np.ndarray:
    return 1.0 - (1.0 - base) * (1.0 - layer)


def radial_glow(
    width: int, height: int, cx: float, cy: float, sigma: float, color: str, strength: float
) -> np.ndarray:
    yy, xx = np.mgrid[0:height, 0:width]
    xn = xx.astype(np.float32) / width
    yn = yy.astype(np.float32) / height
    d2 = ((xn - cx) * (width / height)) ** 2 + (yn - cy) ** 2
    glow = np.exp(-d2 / (2.0 * sigma * sigma))[..., None] * strength
    return np.clip(glow, 0.0, 1.0) * hex_rgb(color)


def save_rgb(array: np.ndarray, path: str) -> None:
    data = np.clip(array * 255.0 + 0.5, 0.0, 255.0).astype(np.uint8)
    Image.fromarray(data, "RGB").save(path, optimize=True)


def make_galaxy(width: int, height: int, path: str) -> None:
    img = vertical_gradient(width, height, "#03040b", "#120826")
    for cx, cy, sigma, color, strength in [
        (0.68, 0.18, 0.34, "#6a2ba8", 0.95),
        (0.83, 0.40, 0.26, "#1d3499", 0.85),
        (0.27, 0.55, 0.33, "#0d5586", 0.55),
        (0.56, 0.88, 0.26, "#0d7078", 0.40),
        (0.12, 0.16, 0.22, "#8a2468", 0.50),
        (0.50, 0.50, 0.18, "#b06ad8", 0.22),
    ]:
        img = screen(img, radial_glow(width, height, cx, cy, sigma, color, strength))

    yy, xx = np.mgrid[0:height, 0:width]
    xn = xx.astype(np.float32) / width
    yn = yy.astype(np.float32) / height

    # A soft diagonal Milky Way with a little structure.
    line = yn - (0.92 - 0.78 * xn)
    band = np.exp(-((line / 0.075) ** 2))
    rng = np.random.default_rng(42)
    small = rng.random((28, 46)).astype(np.float32)
    clump = np.asarray(
        Image.fromarray((small * 255).astype(np.uint8)).resize((width, height), Image.BICUBIC),
        np.float32,
    ) / 255.0
    band *= 0.45 + 0.55 * clump
    img = screen(img, band[..., None] * np.array([0.42, 0.36, 0.72], np.float32) * 0.52)

    # Dense small stars, with a few brighter ones.
    stars = np.zeros((height, width), np.float32)
    for seed, density, brightness in [(11, 0.0022, 0.95), (12, 0.0007, 1.0)]:
        r = np.random.default_rng(seed)
        count = int(width * height * density)
        sx = r.integers(0, width, count)
        sy = r.integers(0, height, count)
        values = r.uniform(0.2, 1.0, count) ** 2
        for px, py, value in zip(sx, sy, values):
            b = 0.55 + 0.45 * band[py, px]
            stars[py, px] = max(stars[py, px], value * b)
    star_img = Image.fromarray((np.clip(stars, 0, 1) * 255).astype(np.uint8))
    star_img = star_img.filter(ImageFilter.GaussianBlur(0.6))
    star_layer = np.asarray(star_img, np.float32) / 255.0
    img = screen(img, star_layer[..., None] * np.array([0.9, 0.95, 1.0], np.float32))

    hero = Image.new("RGB", (width, height), (0, 0, 0))
    draw = ImageDraw.Draw(hero)
    r = random.Random(7)
    for _ in range(40):
        px, py = r.uniform(0, width), r.uniform(0, height)
        radius = r.choice([0.6, 0.8, 1.0, 1.4, 2.1])
        color = r.choice([(255, 255, 255), (215, 230, 255), (255, 235, 205)])
        draw.ellipse((px - radius, py - radius, px + radius, py + radius), fill=color)
    hero = np.asarray(hero.filter(ImageFilter.GaussianBlur(0.35)), np.float32) / 255.0
    img = screen(img, hero)

    # Dither slightly so the 256-colour GIF does not band in the gradients.
    grain = (np.random.default_rng(5).random((height, width, 1), np.float32) - 0.5) * (2.0 / 255.0)
    save_rgb(np.clip(img + grain, 0, 1), path)


def ridge_points(
    seed: int, width: int, height: int, base_frac: float, amplitude_frac: float
) -> list[tuple[int, int]]:
    """A jagged, deterministic ridge line made of alternating peaks and valleys."""
    r = random.Random(seed)
    base = height * base_frac
    amplitude = height * amplitude_frac
    points: list[tuple[int, int]] = [(0, int(base + r.uniform(-amplitude * 0.2, amplitude * 0.2)))]
    x = 0
    while x < width:
        # climb to a peak
        x += r.randint(70, 150)
        peak = base - amplitude * r.uniform(0.45, 1.0)
        points.append((min(x, width), int(peak)))
        if x >= width:
            break
        # drop into a valley
        x += r.randint(55, 125)
        valley = base + amplitude * r.uniform(0.05, 0.42)
        points.append((min(x, width), int(valley)))
    return points


def make_mountains(width: int, height: int, path: str) -> None:
    img = vertical_gradient(width, height, "#23456e", "#f7d7a6")

    yy = np.linspace(0.0, 1.0, height, dtype=np.float32)[:, None]
    horizon = np.clip((yy - 0.45) / 0.55, 0.0, 1.0) ** 1.6
    img = screen(img, horizon[..., None] * hex_rgb("#f0b77a") * 0.28)

    for cx, cy, sigma, color, strength in [
        (0.74, 0.40, 0.32, "#8a6a45", 0.16),
        (0.74, 0.40, 0.15, "#fff1c9", 0.92),
        (0.74, 0.40, 0.052, "#fffdf2", 1.0),
    ]:
        img = screen(img, radial_glow(width, height, cx, cy, sigma, color, strength))

    haze = np.exp(-((yy - 0.68) / 0.075) ** 2) * 0.32
    img = screen(img, haze[..., None] * hex_rgb("#efd6b4"))

    pic = Image.fromarray(np.clip(img * 255.0 + 0.5, 0, 255).astype(np.uint8), "RGB")
    draw = ImageDraw.Draw(pic)
    layers = [
        (0.585, 0.095, "#b7c8d6", 101),
        (0.675, 0.090, "#8fa8bd", 202),
        (0.765, 0.075, "#6a87a1", 303),
        (0.850, 0.058, "#456079", 404),
        (0.920, 0.042, "#273c51", 505),
    ]
    for base_frac, amplitude_frac, color, seed in layers:
        points = ridge_points(seed, width, height, base_frac, amplitude_frac)
        polygon = [(0, height + 2)] + points + [(width, height + 2)]
        draw.polygon(polygon, fill=tuple(int(color[i : i + 2], 16) for i in (1, 3, 5)))

        # A thin fog band on top of each ridge, fading the next layer back.
        fog = Image.new("L", (width, height), 0)
        fog_draw = ImageDraw.Draw(fog)
        ridge_top = min(y for _, y in points)
        fog_draw.rectangle(
            (0, ridge_top - 6, width, ridge_top + int(height * 0.03)),
            fill=52,
        )
        fog = fog.filter(ImageFilter.GaussianBlur(16))
        fog_rgb = Image.new("RGB", (width, height), (232, 240, 246))
        pic = Image.composite(fog_rgb, pic, fog.point(lambda v: int(v * 0.62)))
        draw = ImageDraw.Draw(pic)

    arr = np.asarray(pic, np.float32) / 255.0
    grain = (np.random.default_rng(9).random((height, width, 1), np.float32) - 0.5) * (2.0 / 255.0)
    save_rgb(np.clip(arr + grain, 0, 1), path)


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
