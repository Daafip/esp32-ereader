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
    bionic: bool = False  # word starts drawn bolder (a second time, 1 px to the right)


def bionic_prefix(word: str) -> int:
    """Characters of `word` to embolden: leading punctuation plus the first letters
    (1 for short words, else 40 % rounded up). Same as bionicPrefix() in bookprep.js."""
    letters = sum(1 for c in word if c.isalpha())
    want = 1 if letters <= 3 else -(-letters * 2 // 5)
    seen = 0
    for i, c in enumerate(word):
        if c.isalpha():
            seen += 1
            if seen == want:
                return i + 1
    return 0


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
            text = run.text.replace(NBSP, " ")
            x, baseline = geometry.text_left + run.x, geometry.text_top + y + base
            draw.text((x, baseline), text, font=font, fill=BLACK, anchor="ls")
            if not options.bionic:
                continue
            start = 0
            for word in text.split(" "):  # the same glyphs again, shifted: bolder, same layout
                n = bionic_prefix(word)
                if n:
                    wx = x + font.getlength(text[:start])
                    draw.text((wx + 1, baseline), word[:n], font=font, fill=BLACK, anchor="ls")
                start += len(word) + 1
    # Blank the status-bar band so stray descenders never reach it.
    status_top = geometry.height - geometry.status_bar
    draw.rectangle((0, status_top, geometry.width - 1, geometry.height - 1), fill=WHITE)
    return img.point(lambda v: WHITE if v >= options.threshold else BLACK).convert(
        "1", dither=Image.Dither.NONE
    )


# --- cover ---------------------------------------------------------------------


# 8x8 Bayer matrix for ordered dithering (thresholds (n + 0.5) * 4).
BAYER8 = (
    0, 32, 8, 40, 2, 34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26,
    12, 44, 4, 36, 14, 46, 6, 38, 60, 28, 52, 20, 62, 30, 54, 22,
    3, 35, 11, 43, 1, 33, 9, 41, 51, 19, 59, 27, 49, 17, 57, 25,
    15, 47, 7, 39, 13, 45, 5, 37, 63, 31, 55, 23, 61, 29, 53, 21,
)  # fmt: skip


def dither(
    gray: list[float], w: int, h: int, method: str = "atkinson", level: int = 128
) -> Image.Image:
    """Grey values (0-255, w*h, row by row) to a mode-"1" image. method: "atkinson"
    (passes on 6/8 of the error: clean whites and blacks on a small 1-bit screen),
    "floyd" (Floyd-Steinberg), "ordered" (8x8 Bayer) or "threshold" (at `level`).
    Same as dither() in firmware/web/bookprep.js, bit for bit."""
    err = [float(v) for v in gray]
    img = Image.new("1", (w, h), 1)
    px = img.load()
    for y in range(h):
        for x in range(w):
            i = y * w + x
            if method == "ordered":
                limit = (BAYER8[(y & 7) * 8 + (x & 7)] + 0.5) * 4
            else:
                limit = level if method == "threshold" else 128
            black = err[i] < limit
            if black:
                px[x, y] = 0
            e = err[i] - (0 if black else 255)
            if method == "floyd":
                if x + 1 < w:
                    err[i + 1] += e * 7 / 16
                if y + 1 < h:
                    if x > 0:
                        err[i + w - 1] += e * 3 / 16
                    err[i + w] += e * 5 / 16
                    if x + 1 < w:
                        err[i + w + 1] += e / 16
            elif method == "atkinson":
                a = e / 8
                for dx, dy in ((1, 0), (2, 0), (-1, 1), (0, 1), (1, 1), (0, 2)):
                    if 0 <= x + dx < w and y + dy < h:
                        err[i + dy * w + dx] += a
    return img


def sharpen(gray: list[float], w: int, h: int, amount: float = 0.6) -> list[float]:
    """Unsharp mask with a 3x3 box blur (edges repeat), as sharpen() in bookprep.js:
    a picture scaled down to the screen gets soft, and dithering makes that noise."""
    out = [0.0] * len(gray)
    for y in range(h):
        rows = [min(h - 1, max(0, y + dy)) * w for dy in (-1, 0, 1)]
        for x in range(w):
            cols = [min(w - 1, max(0, x + dx)) for dx in (-1, 0, 1)]
            total = 0.0
            for r in rows:
                for c in cols:
                    total += gray[r + c]
            v = gray[y * w + x]
            out[y * w + x] = min(255.0, max(0.0, v + amount * (v - total / 9)))
    return out


def cover_bitmap(data: bytes, box: tuple[int, int]) -> Image.Image | None:
    """A cover image (any format Pillow reads) scaled to fit `box`, contrast
    stretched, sharpened and Atkinson dithered to 1 bit. None if it can't be read."""
    import io

    from PIL import ImageOps, UnidentifiedImageError

    try:
        img = Image.open(io.BytesIO(data))
        img.load()
    except (UnidentifiedImageError, OSError, ValueError):
        return None
    img = ImageOps.exif_transpose(img)
    if img.mode in ("RGBA", "LA", "P"):
        img = img.convert("RGBA")
        bg = Image.new("RGBA", img.size, (255, 255, 255, 255))
        img = Image.alpha_composite(bg, img)
    img = img.convert("L")
    img.thumbnail(box, Image.Resampling.LANCZOS)
    img = ImageOps.autocontrast(img, cutoff=1)
    w, h = img.size
    return dither(sharpen([float(v) for v in img.tobytes()], w, h), w, h)  # mode L: 1 byte/px


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
