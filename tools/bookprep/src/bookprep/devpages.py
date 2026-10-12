"""Development pages for the firmware: a calibration pattern and two text pages.

Rendered and packed exactly like book pages, then written as a C header
(``firmware/include/test_pages.h``, used by the calibration sketch) and as a small
.t5b (to test the SD path). Run: ``pixi run devpages``.

Calibration pattern (250x122): 1-px border with ticks every 10 px, four different
corner markers (filled square TL, hollow square TR, triangle BL, cross BR), an arrow
pointing at the top edge, and a bit-order staircase: inside every byte the black
pixel steps one row down per pixel, so correct MSB-first bit order shows ``\\\\\\\\``
and a reversed bit order shows ``////``.
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from .cli import default_font
from .extract import Block, Chapter
from .layout import FontMeasurer, Hyphenator, LayoutOptions, PageGeometry, layout_book
from .normalize import normalize_text
from .render import RenderOptions, render_page
from .t5b import TocEntry, pack_page, stride_for, write_t5b

W, H = 250, 122
STAIRCASE_X, STAIRCASE_Y, STAIRCASE_BYTES = 40, 34, 8

# Multatuli, Max Havelaar (1860), opening of chapter 1. Public domain.
SAMPLE_TEXT = [
    "Ik ben makelaar in koffi, en woon op de Lauriergracht, N\N{DEGREE SIGN} 37. Het is myn "
    "gewoonte niet, romans te schryven, of zulke dingen, en het heeft dan ook lang geduurd, "
    "voor ik er toe overging een paar riem papier extra te bestellen, en het werk aantevangen, "
    "dat gy, lieve lezer, zoo-even in de hand hebt genomen, en dat ge lezen moet als ge "
    "makelaar in koffi zyt, of als ge wat anders zyt. Niet alleen dat ik nooit iets schreef "
    "wat naar een roman geleek, maar ik houd er zelfs niet van, iets dergelyks te lezen, omdat "
    "ik een man van zaken ben. Sedert jaren vraag ik my af, waartoe zulke dingen dienen, en ik "
    "sta verbaasd over de onbeschaamdheid, waarmede een dichter of romanverteller u iets op de "
    "mouw durft spelden, dat nooit gebeurd is, en meestal niet gebeuren kan.",
    "Ik zeg: waarheid en gezond verstand, en hier blyf ik by. Voor de Schrift maak ik "
    "natuurlyk een uitzondering. De fout begint al van Van Alphen af, en wel terstond by den "
    "eersten regel over die \N{LEFT DOUBLE QUOTATION MARK}lieve wichtjes."
    "\N{RIGHT DOUBLE QUOTATION MARK} Wat drommel kon dien ouden heer bewegen, zich uittegeven "
    "voor een aanbidder van myn zusje Truitje die zeere oogen had, of van myn "
    "bro\N{LATIN SMALL LETTER E WITH CIRCUMFLEX}r Gerrit die altyd met zyn neus speelde?",
]


def calibration_image(font: ImageFont.FreeTypeFont) -> Image.Image:
    img = Image.new("1", (W, H), 1)
    d = ImageDraw.Draw(img)
    d.rectangle((0, 0, W - 1, H - 1), outline=0)
    for x in range(10, W - 1, 10):  # ticks on the top and bottom edge
        n = 5 if x % 50 == 0 else 2
        d.line((x, 1, x, n), fill=0)
        d.line((x, H - 2, x, H - 1 - n), fill=0)
    for y in range(10, H - 1, 10):  # ticks on the left and right edge
        n = 5 if y % 50 == 0 else 2
        d.line((1, y, n, y), fill=0)
        d.line((W - 2, y, W - 1 - n, y), fill=0)

    d.rectangle((4, 4, 15, 15), fill=0)  # TL
    d.rectangle((W - 16, 4, W - 5, 15), outline=0)  # TR
    d.polygon([(4, H - 5), (15, H - 5), (4, H - 16)], fill=0)  # BL
    d.line((W - 16, H - 16, W - 5, H - 5), fill=0)  # BR
    d.line((W - 16, H - 5, W - 5, H - 16), fill=0)
    d.text((19, 4), "TL", font=font, fill=0)
    d.text((W - 19, 4), "TR", font=font, fill=0, anchor="ra")
    d.text((19, H - 4), "BL", font=font, fill=0, anchor="ls")
    d.text((W - 19, H - 4), "BR", font=font, fill=0, anchor="rs")

    cx = 200  # arrow pointing at the top edge
    d.polygon([(cx, 22), (cx - 9, 36), (cx + 9, 36)], fill=0)
    d.rectangle((cx - 2, 36, cx + 2, 70), fill=0)
    d.text((cx, 74), "UP", font=font, fill=0, anchor="ma")

    for x in range(STAIRCASE_BYTES * 8):
        img.putpixel((STAIRCASE_X + x, STAIRCASE_Y + x % 8), 0)
    d.text((STAIRCASE_X, STAIRCASE_Y + 11), "bit order OK = \\\\\\\\", font=font, fill=0)
    d.text((40, 64), "T5 calibration v1", font=font, fill=0)
    d.text((40, 78), "250x122, stride 32, 1 = white", font=font, fill=0)
    return img


def sample_pages(font_path: Path, size: int = 13) -> list[Image.Image]:
    font = ImageFont.truetype(str(font_path), size, layout_engine=ImageFont.Layout.BASIC)
    geometry = PageGeometry()
    line_height = math.ceil(size * 1.2)
    blocks = [Block("text", normalize_text(t)) for t in SAMPLE_TEXT]
    pages = layout_book(
        [Chapter("Eerste hoofdstuk", blocks)],
        FontMeasurer(font),
        geometry,
        LayoutOptions(line_height=line_height, indent=size),
        Hyphenator("nl"),
    )
    options = RenderOptions(line_height)
    return [render_page(p, font, geometry, options) for p in pages[:2]]


def c_array(name: str, pages: list[bytes]) -> str:
    def rows(data: bytes) -> str:
        lines = [", ".join(f"0x{b:02x}" for b in data[i : i + 16]) for i in range(0, len(data), 16)]
        return ",\n    ".join(lines)

    if len(pages) == 1:
        return f"static const uint8_t {name}[{len(pages[0])}] = {{\n    {rows(pages[0])}\n}};\n"
    body = ",\n".join(f"  {{\n    {rows(p)}\n  }}" for p in pages)
    return f"static const uint8_t {name}[{len(pages)}][{len(pages[0])}] = {{\n{body}\n}};\n"


def write_header(path: Path, calibration: bytes, samples: list[bytes]) -> None:
    text = (
        "// Generated by `pixi run devpages` (tools/bookprep/src/bookprep/devpages.py).\n"
        "// Do not edit. Same bitmap format as .t5b pages, see docs/t5b-format.md.\n"
        "#pragma once\n\n#include <stdint.h>\n\n"
        f"constexpr int kDevPageW = {W};\nconstexpr int kDevPageH = {H};\n"
        f"constexpr int kDevPageBytes = {stride_for(W) * H};\n"
        f"constexpr int kSamplePageCount = {len(samples)};\n\n"
        + c_array("kCalibrationPage", [calibration])
        + "\n"
        + c_array("kSamplePages", samples)
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="ascii")


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="devpages", description=__doc__.splitlines()[0])
    p.add_argument("--header", type=Path, required=True)
    p.add_argument("--t5b", type=Path, required=True)
    args = p.parse_args(argv)

    font_path = default_font()
    if font_path is None:
        p.error("Ubuntu-R.ttf not found; run through pixi")
    label_font = ImageFont.truetype(str(font_path), 11, layout_engine=ImageFont.Layout.BASIC)
    calibration = pack_page(calibration_image(label_font))
    samples = [pack_page(img) for img in sample_pages(font_path)]
    write_header(args.header, calibration, samples)
    args.t5b.parent.mkdir(parents=True, exist_ok=True)
    write_t5b(
        args.t5b,
        title="T5 calibration",
        author="bookprep",
        page_w=W,
        page_h=H,
        toc=[TocEntry(0, "Calibration"), TocEntry(1, "Sample text")],
        pages=[calibration, *samples],
    )
    print(f"wrote {args.header} and {args.t5b} ({1 + len(samples)} pages)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
