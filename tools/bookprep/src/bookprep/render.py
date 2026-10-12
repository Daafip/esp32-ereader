"""Render laid-out pages to 1-bit images, plus preview helpers."""

from __future__ import annotations

import sys
from dataclasses import dataclass
from functools import cache
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from .layout import Page, PageGeometry
from .normalize import NBSP

WHITE = 255
BLACK = 0


@dataclass(frozen=True)
class RenderOptions:
    line_height: int
    threshold: int = 128  # grey < threshold becomes black; higher = bolder
    mono: bool = False  # FreeType monochrome rendering instead of thresholded anti-aliasing


def baseline_offset(font: ImageFont.FreeTypeFont, line_height: int) -> int:
    """Baseline position inside a line box, centring ascent+descent in it."""
    ascent, descent = font.getmetrics()
    return (line_height - (ascent + descent)) // 2 + ascent


def render_page(
    page: Page, font: ImageFont.FreeTypeFont, geometry: PageGeometry, options: RenderOptions
) -> Image.Image:
    img = Image.new("L", (geometry.width, geometry.height), WHITE)
    draw = ImageDraw.Draw(img)
    if options.mono:
        draw.fontmode = "1"
    base = baseline_offset(font, options.line_height)
    for y, line in page.lines:
        for run in line.runs:
            draw.text(
                (geometry.text_left + run.x, geometry.text_top + y + base),
                run.text.replace(NBSP, " "),
                font=font,
                fill=BLACK,
                anchor="ls",
            )
    # Blank the status-bar band so stray descenders never reach it.
    status_top = geometry.height - geometry.status_bar
    draw.rectangle((0, status_top, geometry.width - 1, geometry.height - 1), fill=WHITE)
    return img.point(lambda v: WHITE if v >= options.threshold else BLACK).convert(
        "1", dither=Image.Dither.NONE
    )


# --- previews ----------------------------------------------------------------


@cache
def _status_font(scale: int = 1) -> ImageFont.FreeTypeFont:
    for base in (Path(sys.prefix) / "fonts", Path(sys.prefix) / "Library" / "fonts"):
        if (base / "Ubuntu-M.ttf").exists():
            return ImageFont.truetype(str(base / "Ubuntu-M.ttf"), 10 * scale)
    return ImageFont.load_default(size=9 * scale)


def with_status_bar(
    page_img: Image.Image,
    geometry: PageGeometry,
    left: str,
    right: str,
    progress: float = 0.0,
) -> Image.Image:
    """Approximation of the device's status bar (firmware ui::statusBar), for previews
    only; the .t5b keeps these rows white."""
    img = page_img.convert("L")
    draw = ImageDraw.Draw(img)
    w, h = geometry.width, geometry.height
    s = max(1, round(geometry.status_bar / 14))  # the firmware's UI scale (kUiScale)
    top = h - geometry.status_bar
    x0, x1 = 4 * s, w - 5 * s
    for x in range(x0, x1 + 1, 2 * s):  # dotted track, solid reading progress
        draw.rectangle((x, top + s, x + s - 1, top + 2 * s - 1), fill=BLACK)
    fill = round((x1 - x0 + s) * max(0.0, min(1.0, progress)))
    if fill:
        draw.rectangle((x0, top, x0 + fill - 1, top + 2 * s - 1), fill=BLACK)
    baseline = h - 3 * s
    bx = x1 + s - 16 * s  # battery icon
    draw.rounded_rectangle(
        (bx, baseline - 7 * s, bx + 14 * s - 1, baseline + s - 1), radius=2 * s, outline=BLACK,
        width=s,
    )  # fmt: skip
    draw.rectangle((bx + 14 * s, baseline - 5 * s, bx + 16 * s - 1, baseline - s - 1), fill=BLACK)
    draw.rectangle((bx + 2 * s, baseline - 5 * s, bx + 9 * s - 1, baseline - s - 1), fill=BLACK)
    font = _status_font(s)
    draw.text((bx - 7 * s, baseline), right, font=font, fill=BLACK, anchor="rs")
    right_w = font.getlength(right)
    room = bx - 7 * s - right_w - 12 * s - x0
    text = left
    if font.getlength(text) > room:
        ellipsis = "\N{HORIZONTAL ELLIPSIS}"
        while text and font.getlength(text + ellipsis) > room:
            text = text[:-1]
        text = text.rstrip() + ellipsis if text else ""
    draw.text((x0, baseline), text, font=font, fill=BLACK, anchor="ls")
    return img


def enlarge(img: Image.Image, scale: int, bezel: int = 6) -> Image.Image:
    """Nearest-neighbour upscale with a grey bezel, to judge pixels on a monitor."""
    big = img.convert("L").resize((img.width * scale, img.height * scale), Image.Resampling.NEAREST)
    framed = Image.new("L", (big.width + 2 * bezel, big.height + 2 * bezel), 90)
    framed.paste(big, (bezel, bezel))
    return framed


def contact_sheet(images: list[Image.Image], columns: int = 2, gap: int = 8) -> Image.Image:
    if not images:
        raise ValueError("no pages for the contact sheet")
    w, h = images[0].size
    rows = (len(images) + columns - 1) // columns
    sheet = Image.new("L", (columns * w + (columns + 1) * gap, rows * h + (rows + 1) * gap), 200)
    for i, img in enumerate(images):
        r, c = divmod(i, columns)
        sheet.paste(img.convert("L"), (gap + c * (w + gap), gap + r * (h + gap)))
    return sheet
