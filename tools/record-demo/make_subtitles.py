#!/usr/bin/env python3
"""Turn controller events into an ASS subtitle file for the demo GIF.

    make_subtitles.py OUT.ass EVENTS.tsv WIDTH HEIGHT

Captions sit in the top-left corner; the current theme name sits in the
top-right corner while the theme montage runs.
"""

from __future__ import annotations

import sys
from pathlib import Path

HEADER = """[Script Info]
ScriptType: v4.00+
PlayResX: {width}
PlayResY: {height}
WrapStyle: 2
ScaledBorderAndShadow: yes

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Caption,DejaVu Sans,26,&H00FFFFFF,&H00FFFFFF,&H64000000,&H9A000000,1,0,0,0,100,100,0,0,3,12,0,7,28,28,22,1
Style: Title,DejaVu Sans,34,&H00FFFFFF,&H00FFFFFF,&H64000000,&H9A000000,1,0,0,0,100,100,0,0,3,14,0,8,28,28,40,1
Style: Theme,DejaVu Sans,25,&H0078C8FF,&H00FFFFFF,&H64000000,&H9A000000,1,0,0,0,100,100,1,0,3,12,0,9,28,28,22,1
Style: Cursor,DejaVu Sans,20,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,1,0,0,0,100,100,0,0,1,2,0,7,0,0,0,1
Style: Click,DejaVu Sans,22,&H0000FFFF,&H00FFFFFF,&H00000000,&H00000000,1,0,0,0,100,100,0,0,1,2,0,7,0,0,0,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
"""


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


def dialogue(start: float, end: float, style: str, text: str) -> str:
    if end <= start:
        return ""
    return (
        f"Dialogue: 0,{ass_time(start)},{ass_time(end)},{style},,0,0,0,,"
        f"{{\\fad(140,140)}}{ass_escape(text)}\n"
    )


CURSOR_DRAW = (
    r"m 0 0 l 0 24 l 6 18 l 10 27 l 14 25 l 10 16 l 17 16 l 0 0"
)


def cursor_static(start: float, end: float, x: float, y: float) -> str:
    if end - start <= 0.01:
        return ""
    return (
        f"Dialogue: 0,{ass_time(start)},{ass_time(end)},Cursor,,0,0,0,,"
        f"{{\\an7\\pos({x:.1f},{y:.1f})\\p1\\bord2\\shad0"
        f"\\1c&H00FFFFFF&\\3c&H00000000&}}{CURSOR_DRAW}{{\\p0}}\n"
    )


def cursor_glide(
    start: float, end: float, x1: float, y1: float, x2: float, y2: float
) -> str:
    if end - start <= 0.01:
        return ""
    ms1 = int(start * 1000)
    ms2 = int(end * 1000)
    return (
        f"Dialogue: 0,{ass_time(start)},{ass_time(end)},Cursor,,0,0,0,,"
        f"{{\\an7\\move({x1:.1f},{y1:.1f},{x2:.1f},{y2:.1f},{ms1},{ms2})"
        f"\\p1\\bord2\\shad0\\1c&H00FFFFFF&\\3c&H00000000&}}"
        f"{CURSOR_DRAW}{{\\p0}}\n"
    )


def click_flash(timestamp: float, x: float, y: float) -> str:
    return (
        f"Dialogue: 0,{ass_time(timestamp)},{ass_time(timestamp + 0.22)},Click,,0,0,0,,"
        f"{{\\an7\\pos({x - 8:.1f},{y - 8:.1f})\\fad(0,120)}}\u25cf\n"
    )


def build_cursor(rows: list[tuple[float, str, str]], width: int, height: int,
                 video_end: float) -> list[str]:
    output: list[str] = []
    # Start off-screen; the first real pointer event brings it into view.
    px, py = -100.0, -100.0
    last = 0.0
    for timestamp, kind, text in rows:
        try:
            values = [float(value) for value in text.split(",")]
        except ValueError:
            continue
        if kind == "pointer_move" and len(values) >= 2:
            x, y = values[0] * width, values[1] * height
            output.append(cursor_static(last, timestamp, px, py))
            px, py = x, y
            last = timestamp
        elif kind == "pointer_glide" and len(values) >= 5:
            sx, sy = values[0] * width, values[1] * height
            ex, ey = values[2] * width, values[3] * height
            duration = values[4] / 1000.0
            output.append(cursor_static(last, timestamp, px, py))
            output.append(cursor_glide(timestamp, timestamp + duration, sx, sy, ex, ey))
            px, py = ex, ey
            last = timestamp + duration
        elif kind == "pointer_click" and len(values) >= 2:
            output.append(click_flash(timestamp, values[0] * width, values[1] * height))
    output.append(cursor_static(last, video_end, px, py))
    return output


def main() -> int:
    if len(sys.argv) != 5:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    out_path = Path(sys.argv[1])
    events_path = Path(sys.argv[2])
    width, height = int(sys.argv[3]), int(sys.argv[4])

    rows: list[tuple[float, str, str]] = []
    for raw in events_path.read_text(encoding="utf-8").splitlines():
        parts = raw.split("\t", 2)
        if len(parts) < 2:
            continue
        try:
            timestamp = float(parts[0])
        except ValueError:
            continue
        kind = parts[1]
        text = parts[2] if len(parts) > 2 else ""
        rows.append((timestamp, kind, text))

    lines: list[str] = []
    caption_start: float | None = None
    pending_caption = ""
    theme_start: float | None = None
    theme_text = ""

    for timestamp, kind, text in rows:
        if kind == "caption_start":
            if caption_start is not None:
                style = (
                    "Title"
                    if text.startswith("lambdock --")
                    else "Caption"
                )
                lines.append(dialogue(caption_start, timestamp, style, pending_caption))
            caption_start = timestamp
            pending_caption = text
        elif kind == "caption_end":
            if caption_start is not None:
                style = (
                    "Title"
                    if pending_caption.startswith("lambdock --")
                    else "Caption"
                )
                lines.append(dialogue(caption_start, timestamp, style, pending_caption))
                caption_start = None
        elif kind == "theme":
            if theme_start is not None:
                lines.append(dialogue(theme_start, timestamp, "Theme", theme_text))
            theme_start = timestamp
            theme_text = f"theme: {text}"
        elif kind == "theme_end":
            if theme_start is not None:
                lines.append(dialogue(theme_start, timestamp, "Theme", theme_text))
                theme_start = None

    # Close any event left open by an interrupted run.
    last_time = rows[-1][0] if rows else 0.0
    if caption_start is not None:
        lines.append(dialogue(caption_start, last_time + 1.5, "Caption", pending_caption))
    if theme_start is not None:
        lines.append(dialogue(theme_start, last_time + 1.5, "Theme", theme_text))

    lines.extend(build_cursor(rows, width, height, last_time + 2.0))

    out_path.write_text(
        HEADER.format(width=width, height=height) + "".join(lines),
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
