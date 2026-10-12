"""bookprep: EPUB/TXT -> .t5b (+ preview PNGs and a report)."""

from __future__ import annotations

import argparse
import math
import re
import sys
from pathlib import Path

from PIL import ImageFont

from . import __version__
from .extract import Block, Book, Chapter, extract
from .layout import FontMeasurer, Hyphenator, LayoutOptions, Page, PageGeometry, layout_book
from .normalize import GlyphFallback, font_charset, normalize_text
from .render import RenderOptions, contact_sheet, enlarge, render_page, with_status_bar
from .t5b import TocEntry, pack_page, write_t5b

DEFAULT_LANG = "nl"


def default_font() -> Path | None:
    """Ubuntu Regular from the pixi env (conda-forge font-ttf-ubuntu), if present."""
    for base in (Path(sys.prefix) / "fonts", Path(sys.prefix) / "Library" / "fonts"):
        path = base / "Ubuntu-R.ttf"
        if path.exists():
            return path
    return None


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    p = argparse.ArgumentParser(prog="bookprep", description=__doc__)
    p.add_argument("input", type=Path, help="DRM-free .epub or .txt")
    p.add_argument("--font", type=Path, help="TTF/OTF file (default: Ubuntu from the pixi env)")
    p.add_argument("--size", type=int, default=13, help="font size in pixels (default 13)")
    p.add_argument("--line-height", type=int, help="pixels per line (default: 1.2 x size)")
    p.add_argument("--lang", help="hyphenation language (default: EPUB metadata, else nl)")
    p.add_argument("--out", type=Path, default=Path("out"), help="output directory")
    p.add_argument("--margin", type=int, default=4, help="left/right/top margin in px")
    p.add_argument("--status-bar", type=int, default=10, help="rows reserved for the status bar")
    p.add_argument("--indent", type=int, help="first-line indent in px (default: size)")
    p.add_argument("--paragraph-spacing", type=int, default=0, help="extra px between paragraphs")
    p.add_argument("--no-justify", action="store_true", help="ragged right instead of justified")
    p.add_argument("--no-hyphenate", action="store_true")
    p.add_argument("--no-chapter-break", action="store_true", help="no page break per chapter")
    p.add_argument("--threshold", type=int, default=128, help="AA grey -> black cut-off (0-255)")
    p.add_argument("--mono", action="store_true", help="FreeType mono rendering, no threshold")
    p.add_argument("--title", help="override the title")
    p.add_argument("--author", help="override the author")
    p.add_argument("--preview", action="store_true", help="write preview PNGs + contact sheet")
    p.add_argument("--preview-pages", type=int, default=10)
    p.add_argument("--preview-start", type=int, default=0, help="first page to preview (0-based)")
    p.add_argument("--version", action="version", version=f"bookprep {__version__}")
    return p.parse_args(argv)


def slug(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]+", "_", name).strip("_") or "book"


def clean_book(book: Book, fix) -> Book:
    """Normalise every string in the book and map characters missing from the font."""

    def clean(text: str) -> str:
        return fix(normalize_text(text))

    chapters = [
        Chapter(
            clean(c.title) if c.title else None,
            [Block(b.kind, clean(b.text)) for b in c.blocks],
        )
        for c in book.chapters
    ]
    return Book(clean(book.title), clean(book.author), book.language, chapters)


def toc_entries(chapters: list[Chapter], pages: list[Page]) -> list[TocEntry]:
    first_page: dict[int, int] = {}
    for n, page in enumerate(pages):
        for _, line in page.lines:
            first_page.setdefault(line.chapter, n)
    return [
        TocEntry(first_page[ci], chapter.title)
        for ci, chapter in enumerate(chapters)
        if chapter.title and ci in first_page
    ]


def chapter_title_at(toc: list[TocEntry], page: int) -> str:
    title = ""
    for entry in toc:
        if entry.start_page <= page:
            title = entry.title
    return title


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    font_path = args.font or default_font()
    if font_path is None or not font_path.exists():
        print("error: no font; pass --font path/to/font.ttf", file=sys.stderr)
        return 2
    if not args.input.exists():
        print(f"error: {args.input} not found", file=sys.stderr)
        return 2

    raw = extract(args.input)
    fallback = GlyphFallback.for_charset(font_charset(font_path))
    book = clean_book(raw, fallback)
    title = args.title or book.title
    author = args.author if args.author is not None else book.author

    lang = args.lang or raw.language or DEFAULT_LANG
    hyphenator = None if args.no_hyphenate else Hyphenator(lang)
    if hyphenator is not None and hyphenator.lang is None:
        print(f"warning: no hyphenation dictionary for {lang!r}; hyphenation off")
        hyphenator = None

    font = ImageFont.truetype(str(font_path), args.size, layout_engine=ImageFont.Layout.BASIC)
    line_height = args.line_height or math.ceil(args.size * 1.2)
    geometry = PageGeometry(
        margin_left=args.margin,
        margin_right=args.margin,
        margin_top=args.margin,
        status_bar=args.status_bar,
    )
    layout_opts = LayoutOptions(
        line_height=line_height,
        indent=args.size if args.indent is None else args.indent,
        paragraph_spacing=args.paragraph_spacing,
        justify=not args.no_justify,
        chapter_page_break=not args.no_chapter_break,
    )
    measurer = FontMeasurer(font, mode="1" if args.mono else "")
    pages = layout_book(book.chapters, measurer, geometry, layout_opts, hyphenator)
    if not pages:
        print("error: the book has no text", file=sys.stderr)
        return 1

    render_opts = RenderOptions(line_height, threshold=args.threshold, mono=args.mono)
    images = [render_page(page, font, geometry, render_opts) for page in pages]
    toc = toc_entries(book.chapters, pages)

    args.out.mkdir(parents=True, exist_ok=True)
    stem = slug(args.input.stem)
    t5b_path = args.out / f"{stem}.t5b"
    header = write_t5b(
        t5b_path,
        title=title,
        author=author,
        page_w=geometry.width,
        page_h=geometry.height,
        toc=toc,
        pages=(pack_page(img) for img in images),
    )

    lines_per_page = geometry.text_height // line_height
    full_lines = [
        line.text
        for page in pages
        for _, line in page.lines
        if line.index < line.count - 1 and line.runs
    ]
    avg_chars = sum(map(len, full_lines)) / max(len(full_lines), 1)
    report = [
        f"bookprep {__version__}",
        f"input:    {args.input}",
        f"output:   {t5b_path} ({t5b_path.stat().st_size / 1024:.0f} KB)",
        f"title:    {title}",
        f"author:   {author}",
        f"language: {lang} (hyphenation: {hyphenator.lang if hyphenator else 'off'})",
        f"font:     {font_path.name} {args.size}px, line height {line_height}px, "
        f"{'mono' if args.mono else f'threshold {args.threshold}'}",
        f"page:     {geometry.width}x{geometry.height}, text area "
        f"{geometry.text_width}x{geometry.text_height} at ({geometry.text_left},"
        f"{geometry.text_top}), status bar {geometry.status_bar}px",
        f"layout:   {lines_per_page} lines/page, ~{avg_chars:.0f} chars/line, "
        f"{'justified' if layout_opts.justify else 'ragged'}, indent {layout_opts.indent}px",
        f"pages:    {header.page_count}, chapters {len(book.chapters)}, TOC entries {len(toc)}",
        "toc:",
        *(f"  p{e.start_page + 1:>4}  {e.title}" for e in toc),
        "characters:",
        *(fallback.report() or ["  all characters present in the font"]),
    ]
    report_path = args.out / f"{stem}.report.txt"
    report_path.write_text("\n".join(report) + "\n", encoding="utf-8")
    print("\n".join(report[:10]))
    if fallback.missing:
        print(f"warning: {sum(fallback.missing.values())} characters missing from the font")
    print(f"report:   {report_path}")

    if args.preview:
        start = min(max(args.preview_start, 0), len(images) - 1)
        chosen = range(start, min(start + args.preview_pages, len(images)))
        preview_dir = args.out / f"{stem}.preview"
        preview_dir.mkdir(exist_ok=True)
        framed = []
        for n in chosen:
            status = with_status_bar(
                images[n],
                geometry,
                chapter_title_at(toc, n)[:32],
                f"{n + 1}/{len(images)}  {100 * (n + 1) // len(images)}%",
            )
            enlarge(status, 3).save(preview_dir / f"page-{n + 1:04d}.png")
            framed.append(enlarge(status, 2))
        sheet_path = args.out / f"{stem}.contact.png"
        contact_sheet(framed).save(sheet_path)
        print(f"preview:  {preview_dir}/ ({len(framed)} pages), {sheet_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
