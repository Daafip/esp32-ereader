"""Render laid-out pages to 1-bit images, plus preview helpers."""

from __future__ import annotations

from dataclasses import dataclass

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


def with_status_bar(page_img: Image.Image, geometry: PageGeometry, left: str, right: str):
    """Mock of the on-device status bar, for previews only (not stored in .t5b)."""
    img = page_img.convert("L")
    draw = ImageDraw.Draw(img)
    top = geometry.height - geometry.status_bar
    draw.line((0, top, geometry.width - 1, top), fill=BLACK)
    font = ImageFont.load_default(size=8)
    draw.text((2, top + 1), left, font=font, fill=BLACK)
    draw.text((geometry.width - 2, top + 1), right, font=font, fill=BLACK, anchor="ra")
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
