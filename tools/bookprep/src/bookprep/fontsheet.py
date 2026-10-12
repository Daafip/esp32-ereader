"""Font x size comparison sheet for picking the reading font (Checkpoint 2).

Renders the same page of a book in every combination of --font and --sizes, with
the device's status bar mocked, into one PNG:

    pixi run font-sheet book.epub --font a.ttf --font b.ttf --sizes 11 12 13 --mono
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from .cli import clean_book, default_font
from .extract import Book, extract
from .layout import FontMeasurer, Hyphenator, LayoutOptions, PageGeometry, layout_book
from .normalize import GlyphFallback, font_charset
from .render import RenderOptions, enlarge, render_page, with_status_bar


def sample_chapter(book: Book):
    """The chapter with the most running text (body text, not front matter)."""
    return max(book.chapters, key=lambda c: sum(len(b.text) for b in c.blocks if b.kind == "text"))


def tile(raw: Book, chapter, font_path: Path, size: int, mono: bool, threshold: int,
         lang: str, label_font) -> Image.Image:  # fmt: skip
    one = Book(raw.title, raw.author, raw.language, [chapter])
    book = clean_book(one, GlyphFallback.for_charset(font_charset(font_path)))
    font = ImageFont.truetype(str(font_path), size, layout_engine=ImageFont.Layout.BASIC)
    geometry = PageGeometry()
    line_height = math.ceil(size * 1.2)
    pages = layout_book(
        book.chapters,
        FontMeasurer(font, mode="1" if mono else ""),
        geometry,
        LayoutOptions(line_height=line_height, indent=size),
        Hyphenator(lang),
    )
    page = max(pages[:12], key=lambda p: sum(len(line.text) for _, line in p.lines))
    img = render_page(page, font, geometry, RenderOptions(line_height, threshold, mono))
    img = enlarge(with_status_bar(img, geometry, chapter.title or "", "27/412  6%"), 2)
    canvas = Image.new("L", (img.width, img.height + 30), 255)
    canvas.paste(img, (0, 30))
    lines = geometry.text_height // line_height
    label = f"{font_path.stem} {size}px, {lines} lines, {'mono' if mono else f't{threshold}'}"
    ImageDraw.Draw(canvas).text((4, 2), label, font=label_font, fill=0)
    return canvas


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="font-sheet", description=__doc__.splitlines()[0])
    p.add_argument("input", type=Path, help=".epub or .txt")
    p.add_argument("--font", type=Path, action="append", help="repeatable; default Ubuntu")
    p.add_argument("--sizes", type=int, nargs="+", default=[11, 12, 13, 14])
    p.add_argument("--mono", action="store_true", help="FreeType mono rendering")
    p.add_argument("--threshold", type=int, default=128)
    p.add_argument("--lang", default=None, help="hyphenation (default: book, else nl)")
    p.add_argument("--out", type=Path, default=Path("out/font-sheet.png"))
    args = p.parse_args(argv)

    fonts = args.font or [default_font()]
    raw = extract(args.input)
    chapter = sample_chapter(raw)
    lang = args.lang or raw.language or "nl"
    label_font = ImageFont.truetype(str(default_font()), 22)
    tiles = [
        tile(raw, chapter, f, s, args.mono, args.threshold, lang, label_font)
        for f in fonts
        for s in args.sizes
    ]
    w, h = tiles[0].size
    cols = len(args.sizes)
    rows = math.ceil(len(tiles) / cols)
    gap = 12
    sheet = Image.new("L", (cols * w + (cols + 1) * gap, rows * h + (rows + 1) * gap), 220)
    for i, t in enumerate(tiles):
        r, c = divmod(i, cols)
        sheet.paste(t, (gap + c * (w + gap), gap + r * (h + gap)))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(args.out)
    print(f"{args.out} ({len(fonts)} fonts x {len(args.sizes)} sizes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
