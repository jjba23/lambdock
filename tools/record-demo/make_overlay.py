#!/usr/bin/env python3
# lambdock -- hyper-hackable desktop dock for Wayland
#
# SPDX-License-Identifier: MIT
# Copyright © Josep Bigorra <jjbigorra@gmail.com>

"""Build the presentation layer of the demo GIF: backdrop, mask and captions.

    make_overlay.py --events EVENTS.tsv --outdir DIR \
                    --screen WxH --margin PX --duration SECONDS

The recording is not shown edge to edge.  It is inset in a slightly larger
canvas, clipped to a rounded rectangle and laid on a soft drop shadow, which is
what makes the result read like a product page video instead of a raw screen
grab.  Three files come out of here:

  frame.png     the static backdrop with the shadow painted in
  mask.png      a greyscale rounded rectangle, used with alphamerge
  captions.ass  captions, theme labels and title cards, positioned in canvas
                coordinates so they never depend on the compositor layout

Captions are drawn as their own rounded pill so the text stays legible over any
wallpaper, and they sit at the top of the screen (the dock owns the bottom).
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

# Presentation constants.  Everything is in output (canvas) pixels.
CORNER_RADIUS = 18
PILL_PAD_X = 20
PILL_HEIGHT = 38
PILL_RADIUS = 19
THEME_HEIGHT = 29
THEME_RADIUS = 14
CAPTION_TOP = 20
TEXT_SIZE = 22
THEME_SIZE = 16
TITLE_SIZE = 46
SUBTITLE_SIZE = 22

FONT_FAMILY = "Noto Sans"
FONT_BOLD = "/usr/share/fonts/truetype/NotoSans-Bold.ttf"
FONT_REGULAR = "/usr/share/fonts/truetype/NotoSans-Regular.ttf"

# ASS colours are &HAABBGGRR (alpha 00 = opaque, FF = transparent).
WHITE = "&H00FFFFFF&"
SOFT_WHITE = "&H26FFFFFF&"
ACCENT = "&H00FFD9A8&"
PILL = "&H00000000&"
PILL_ALPHA = "&H3A&"
THEME_PILL_ALPHA = "&H4A&"


# ------------------------------------------------------------------ utilities
def ass_time(seconds: float) -> str:
    seconds = max(0.0, seconds)
    hours = int(seconds // 3600)
    minutes = int((seconds % 3600) // 60)
    secs = seconds % 60
    return f"{hours}:{minutes:02d}:{secs:05.2f}"


def ass_escape(text: str) -> str:
    return (
        text.replace("\\", "\\\\")
        .replace("{", "\\{")
        .replace("}", "\\}")
        .replace("\n", "\\N")
    )


def font(size: int, bold: bool) -> ImageFont.FreeTypeFont:
    path = FONT_BOLD if bold else FONT_REGULAR
    try:
        return ImageFont.truetype(path, size)
    except OSError:
        return ImageFont.load_default()


_MEASURE = {}


def text_width(text: str, size: int, bold: bool = True) -> int:
    key = (size, bold)
    if key not in _MEASURE:
        _MEASURE[key] = font(size, bold)
    measured = _MEASURE[key]
    box = measured.getbbox(text)
    return int(math.ceil(box[2] - box[0]))


def rounded_rect_path(
    x: float, y: float, width: float, height: float, radius: float
) -> str:
    """An ASS drawing command for a rounded rectangle.

    ASS has no rounded-rectangle primitive, so the corners are cubic beziers
    with the usual 0.5523 circle approximation.  The path is in shape-local
    coordinates: pair it with \\an7 and the top-left corner in \\pos().
    """
    k = 0.5523
    r = min(radius, width / 2.0, height / 2.0)
    right = x + width
    bottom = y + height
    return (
        f"m {x + r:.1f} {y:.1f} "
        f"l {right - r:.1f} {y:.1f} "
        f"b {right - r + r * k:.1f} {y:.1f} "
        f"{right:.1f} {y + r - r * k:.1f} {right:.1f} {y + r:.1f} "
        f"l {right:.1f} {bottom - r:.1f} "
        f"b {right:.1f} {bottom - r + r * k:.1f} "
        f"{right - r + r * k:.1f} {bottom:.1f} {right - r:.1f} {bottom:.1f} "
        f"l {x + r:.1f} {bottom:.1f} "
        f"b {x + r - r * k:.1f} {bottom:.1f} "
        f"{x:.1f} {bottom - r + r * k:.1f} {x:.1f} {bottom - r:.1f} "
        f"l {x:.1f} {y + r:.1f} "
        f"b {x:.1f} {y + r - r * k:.1f} "
        f"{x + r - r * k:.1f} {y:.1f} {x + r:.1f} {y:.1f}"
    )


# -------------------------------------------------------------------- artwork
def vertical_gradient(width: int, height: int, top: str, bottom: str) -> Image.Image:
    top_rgb = tuple(int(top[i : i + 2], 16) for i in (1, 3, 5))
    bottom_rgb = tuple(int(bottom[i : i + 2], 16) for i in (1, 3, 5))
    column = Image.new("RGB", (1, height))
    draw = ImageDraw.Draw(column)
    for y in range(height):
        t = y / max(1, height - 1)
        draw.point(
            (0, y),
            tuple(
                round(top_rgb[channel] * (1.0 - t) + bottom_rgb[channel] * t)
                for channel in range(3)
            ),
        )
    return column.resize((width, height), Image.NEAREST)


def radial_glow(
    width: int, height: int, cx: float, cy: float, radius: float, alpha: int
) -> Image.Image:
    """A soft additive highlight, used to keep the flat backdrop from deadness."""
    small = Image.new("L", (width // 4, height // 4), 0)
    draw = ImageDraw.Draw(small)
    sx, sy, sr = cx / 4.0, cy / 4.0, radius / 4.0
    steps = 18
    for step in range(steps, 0, -1):
        t = step / steps
        value = int(alpha * (1.0 - t) ** 2)
        draw.ellipse(
            (sx - sr * t, sy - sr * t, sx + sr * t, sy + sr * t), fill=value
        )
    return small.filter(ImageFilter.GaussianBlur(radius=radius / 12.0)).resize(
        (width, height), Image.BICUBIC
    )


def make_frame(canvas_w: int, canvas_h: int, screen: tuple[int, int], margin: int) -> Image.Image:
    base = vertical_gradient(canvas_w, canvas_h, "#0B0D13", "#191C25").convert("RGBA")
    glow = radial_glow(
        canvas_w, canvas_h, canvas_w * 0.5, canvas_h * 0.10, canvas_w * 0.62, 26
    )
    base = Image.composite(
        Image.new("RGBA", base.size, (58, 74, 122, 255)),
        base,
        glow,
    )

    # Shadow and hairline border are drawn supersampled: PIL does not antialias
    # shapes, and a stair-stepped shadow reads as a cheap GIF.
    scale = 3
    layer = Image.new("RGBA", (canvas_w * scale, canvas_h * scale), (0, 0, 0, 0))
    for blur, offset, alpha in ((10, 4, 150), (34, 16, 120)):
        shadow = Image.new("RGBA", layer.size, (0, 0, 0, 0))
        draw = ImageDraw.Draw(shadow)
        draw.rounded_rectangle(
            (
                (margin - 2) * scale,
                (margin - 2 + offset) * scale,
                (margin + screen[0] + 2) * scale,
                (margin + screen[1] + 2 + offset) * scale,
            ),
            radius=(CORNER_RADIUS + 2) * scale,
            fill=(0, 0, 0, alpha),
        )
        layer = Image.alpha_composite(layer, shadow.filter(ImageFilter.GaussianBlur(blur * scale)))

    border = Image.new("RGBA", layer.size, (0, 0, 0, 0))
    ImageDraw.Draw(border).rounded_rectangle(
        (
            margin * scale,
            margin * scale,
            (margin + screen[0]) * scale - 1,
            (margin + screen[1]) * scale - 1,
        ),
        radius=CORNER_RADIUS * scale,
        outline=(255, 255, 255, 60),
        width=int(1.3 * scale),
    )
    layer = Image.alpha_composite(layer, border)
    return Image.alpha_composite(
        base, layer.resize((canvas_w, canvas_h), Image.LANCZOS)
    ).convert("RGB")


def make_mask(width: int, height: int) -> Image.Image:
    # Supersample so the rounded corners stay clean after the GIF's palette pass.
    # The mask is a hair smaller than the screen rectangle: the one pixel it
    # gives up is where the backdrop's border shows around the recording.
    scale = 4
    hi = Image.new("L", (width * scale, height * scale), 0)
    inset = int(1.5 * scale)
    draw = ImageDraw.Draw(hi)
    draw.rounded_rectangle(
        (inset, inset, width * scale - 1 - inset, height * scale - 1 - inset),
        radius=CORNER_RADIUS * scale - inset,
        fill=255,
    )
    return hi.resize((width, height), Image.LANCZOS)


# ------------------------------------------------------------------ subtitles
HEADER = """[Script Info]
ScriptType: v4.00+
PlayResX: {width}
PlayResY: {height}
WrapStyle: 2
ScaledBorderAndShadow: yes
Kerning: yes

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Cap,{family},{cap_size},{white},{white},&H00000000&,&H00000000&,-1,0,0,0,100,100,0.2,0,1,0,0,5,0,0,0,1
Style: Theme,{family},{theme_size},&H00FFE0BE&,&H00FFFFFF&,&H00000000&,&H00000000&,-1,0,0,0,100,100,0.2,0,1,0,0,5,0,0,0,1
Style: Title,{family},{title_size},{white},{white},&H00000000&,&H00000000&,-1,0,0,0,100,100,0.4,0,1,0,0,5,0,0,0,1
Style: Subtitle,{family},{subtitle_size},{soft_white},{white},&H00000000&,&H00000000&,0,0,0,0,100,100,0.4,0,1,0,0,5,0,0,0,1
Style: Art,{family},20,&H00FFFFFF&,&H00FFFFFF&,&H00FFFFFF&,&H00000000&,0,0,0,0,100,100,0,0,1,0,0,7,0,0,0,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
"""


class SubtitleWriter:
    def __init__(self, width: int, height: int, margin: int, screen: tuple[int, int]):
        self.width = width
        self.height = height
        self.margin = margin
        self.screen_w, self.screen_h = screen
        self.lines: list[str] = []

    # -- primitives
    def art(self, start: float, end: float, body: str, layer: int = 0) -> None:
        if end <= start:
            return
        self.lines.append(
            f"Dialogue: {layer},{ass_time(start)},{ass_time(end)},Art,,0,0,0,,{body}\n"
        )

    def pill(
        self,
        start: float,
        end: float,
        left: float,
        top: float,
        width: float,
        height: float,
        alpha: str = PILL_ALPHA,
        fade: tuple[int, int] = (170, 170),
        rise: float = 7.0,
    ) -> None:
        path = rounded_rect_path(0, 0, width, height, min(height / 2.0, THEME_RADIUS * 2))
        motion = (
            f"\\move({left:.1f},{top + rise:.1f},{left:.1f},{top:.1f},0,170)"
            if rise
            else ""
        )
        self.art(
            start,
            end,
            f"{{\\an7\\pos({left:.1f},{top:.1f}){motion}\\fad({fade[0]},{fade[1]})"
            f"\\p1\\bord0\\shad0\\1c{PILL}\\1a{alpha}}}{path}{{\\p0}}",
        )

    def text(
        self,
        start: float,
        end: float,
        cx: float,
        cy: float,
        body: str,
        style: str = "Cap",
        layer: int = 1,
        fade: tuple[int, int] = (170, 170),
        rise: float = 7.0,
        colour: str | None = None,
    ) -> None:
        if end <= start:
            return
        motion = (
            f"\\move({cx:.1f},{cy + rise:.1f},{cx:.1f},{cy:.1f},0,170)" if rise else ""
        )
        tint = f"\\1c{colour}" if colour else ""
        self.lines.append(
            f"Dialogue: {layer},{ass_time(start)},{ass_time(end)},{style},,0,0,0,,"
            f"{{\\an5\\pos({cx:.1f},{cy:.1f}){motion}\\fad({fade[0]},{fade[1]}){tint}}}"
            f"{ass_escape(body)}\n"
        )

    def label(self, start: float, end: float, text: str, right: bool = True) -> None:
        width = text_width(text, THEME_SIZE, True) + 2 * PILL_PAD_X
        top = self.margin + CAPTION_TOP + (PILL_HEIGHT - THEME_HEIGHT) / 2.0
        cx = (
            self.width - self.margin - 18 - width / 2.0
            if right
            else self.margin + 18 + width / 2.0
        )
        self.pill(
            start,
            end,
            cx - width / 2.0,
            top,
            width,
            THEME_HEIGHT,
            alpha=THEME_PILL_ALPHA,
            fade=(70, 70),
            rise=4.0,
        )
        self.text(
            start,
            end,
            cx,
            top + THEME_HEIGHT / 2.0 - 1,
            text,
            style="Theme",
            layer=1,
            fade=(70, 70),
            rise=4.0,
            colour=ACCENT,
        )

    def caption(self, start: float, end: float, text: str) -> None:
        width = text_width(text, TEXT_SIZE, True) + 2 * PILL_PAD_X
        top = self.margin + CAPTION_TOP
        left = (self.width - width) / 2.0
        self.pill(start, end, left, top, width, PILL_HEIGHT)
        self.text(
            start,
            end,
            self.width / 2.0,
            top + PILL_HEIGHT / 2.0 - 1,
            text,
            layer=1,
        )

    def dim(self, start: float, end: float, alpha: int = 122, fade=(400, 400)) -> None:
        path = rounded_rect_path(0, 0, self.screen_w, self.screen_h, CORNER_RADIUS)
        self.art(
            start,
            end,
            f"{{\\an7\\pos({self.margin},{self.margin})\\p1\\bord0\\shad0"
            f"\\1c&H100C08&\\1a&H{alpha:02X}&\\fad({fade[0]},{fade[1]})}}{path}{{\\p0}}",
        )

    def title_card(
        self, start: float, end: float, title: str, subtitle: str = ""
    ) -> None:
        self.dim(start, end)
        cx = self.width / 2.0
        cy = self.height / 2.0
        dy = -18 if subtitle else 0
        self.text(start, end, cx, cy + dy, title, style="Title", fade=(420, 380), rise=0)
        if subtitle:
            self.text(
                start,
                end,
                cx,
                cy + 42,
                subtitle,
                style="Subtitle",
                colour=SOFT_WHITE,
                fade=(420, 380),
                rise=0,
            )

    def fade_in(self, end: float) -> None:
        self.art(
            0.0,
            end,
            f"{{\\an7\\pos(0,0)\\p1\\bord0\\shad0\\1c&H000000&\\fad({int(end * 1000)},0)}}"
            f"m 0 0 l {self.width} 0 l {self.width} {self.height} l 0 {self.height}",
            layer=5,
        )

    def fade_out(self, start: float, end: float) -> None:
        self.art(
            start,
            end,
            f"{{\\an7\\pos(0,0)\\p1\\bord0\\shad0\\1c&H000000&\\fad(0,{int((end - start) * 1000)})}}"
            f"m 0 0 l {self.width} 0 l {self.width} {self.height} l 0 {self.height}",
            layer=5,
        )


# ------------------------------------------------------------------- assembly
def read_events(path: Path) -> list[tuple[float, str, str]]:
    rows: list[tuple[float, str, str]] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        parts = raw.split("\t", 2)
        if len(parts) < 2:
            continue
        try:
            timestamp = float(parts[0])
        except ValueError:
            continue
        rows.append((timestamp, parts[1], parts[2] if len(parts) > 2 else ""))
    return rows


def build_subtitles(
    rows: list[tuple[float, str, str]],
    writer: SubtitleWriter,
    duration: float,
) -> None:
    caption: tuple[float, str] | None = None
    theme: tuple[float, str] | None = None
    title: tuple[float, str, str] | None = None

    for timestamp, kind, text in rows:
        if kind == "caption_start":
            if caption:
                writer.caption(caption[0], timestamp, caption[1])
            caption = (timestamp, text)
        elif kind == "caption_end":
            if caption:
                writer.caption(caption[0], timestamp, caption[1])
                caption = None
        elif kind == "theme":
            if theme:
                writer.label(theme[0], timestamp, theme[1])
            theme = (timestamp, text)
        elif kind == "theme_end":
            if theme:
                writer.label(theme[0], timestamp, theme[1])
                theme = None
        elif kind == "title":
            if title:
                writer.title_card(title[0], timestamp, title[1], title[2])
            head, _, sub = text.partition("\t")
            title = (timestamp, head, sub)
        elif kind == "title_end":
            if title:
                writer.title_card(title[0], timestamp, title[1], title[2])
                title = None

    # An interrupted run leaves an event open; close it at the end of the video.
    if caption:
        writer.caption(caption[0], min(duration, caption[0] + 3.0), caption[1])
    if theme:
        writer.label(theme[0], min(duration, theme[0] + 1.0), theme[1])
    if title:
        writer.title_card(title[0], duration, title[1], title[2])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--events", required=True)
    parser.add_argument("--outdir", required=True)
    parser.add_argument("--screen", required=True, help="WxH of the recording area")
    parser.add_argument("--margin", type=int, default=26)
    parser.add_argument("--duration", type=float, required=True)
    args = parser.parse_args()

    screen_w, screen_h = (int(value) for value in args.screen.lower().split("x"))
    margin = args.margin
    canvas_w = screen_w + 2 * margin
    canvas_h = screen_h + 2 * margin

    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    make_frame(canvas_w, canvas_h, (screen_w, screen_h), margin).save(
        outdir / "frame.png", optimize=True
    )
    make_mask(screen_w, screen_h).save(outdir / "mask.png", optimize=True)

    writer = SubtitleWriter(canvas_w, canvas_h, margin, (screen_w, screen_h))
    build_subtitles(read_events(Path(args.events)), writer, args.duration)
    writer.fade_in(0.9)
    writer.fade_out(max(0.0, args.duration - 0.9), args.duration)

    header = HEADER.format(
        width=canvas_w,
        height=canvas_h,
        family=FONT_FAMILY,
        cap_size=TEXT_SIZE,
        theme_size=THEME_SIZE,
        title_size=TITLE_SIZE,
        subtitle_size=SUBTITLE_SIZE,
        white=WHITE,
        soft_white=SOFT_WHITE,
    )
    (outdir / "captions.ass").write_text(header + "".join(writer.lines), encoding="utf-8")
    print(f"presentation layer: {canvas_w}x{canvas_h} canvas, {screen_w}x{screen_h} screen")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
