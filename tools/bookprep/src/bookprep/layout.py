"""Line breaking and pagination.

Greedy line breaking on measured word widths, with pyphen hyphenation when a word
does not fit and optional justification. Pagination keeps headings with the text
that follows and applies light widow/orphan control (no lone first or last line
of a paragraph at a page edge).
"""

from __future__ import annotations

import re
from collections import deque
from dataclasses import dataclass, field
from typing import Protocol

import pyphen

from .extract import Chapter
from .normalize import NBSP

SCENE_BREAK = "* * *"


class Measurer(Protocol):
    def width(self, text: str) -> float: ...


class FontMeasurer:
    """Advance widths from a Pillow FreeType font, cached per string.

    ``mode`` must match how the text is rendered: "1" (FreeType mono hinting) has
    different, usually wider, advances than anti-aliased rendering.
    """

    def __init__(self, font, mode: str = ""):
        self.font = font
        self.mode = mode
        self._cache: dict[str, float] = {}

    def width(self, text: str) -> float:
        w = self._cache.get(text)
        if w is None:
            w = self._cache[text] = self.font.getlength(text.replace(NBSP, " "), mode=self.mode)
        return w


@dataclass(frozen=True)
class PageGeometry:
    """Page size and the text area inside it (all pixels).

    The bottom ``status_bar`` rows are left white; the firmware draws the status
    bar there.
    """

    width: int = 250
    height: int = 122
    margin_left: int = 4
    margin_right: int = 4
    margin_top: int = 4
    margin_bottom: int = 4
    status_bar: int = 14

    @property
    def text_left(self) -> int:
        return self.margin_left

    @property
    def text_top(self) -> int:
        return self.margin_top

    @property
    def text_width(self) -> int:
        return self.width - self.margin_left - self.margin_right

    @property
    def text_height(self) -> int:
        return self.height - self.margin_top - self.margin_bottom - self.status_bar


@dataclass(frozen=True)
class LayoutOptions:
    line_height: int
    indent: int = 0
    paragraph_spacing: int = 0
    justify: bool = True
    chapter_page_break: bool = True
    widow_orphan: bool = True


@dataclass(frozen=True)
class Run:
    x: float  # offset from the text area's left edge
    text: str


@dataclass
class Line:
    runs: list[Run]
    chapter: int = 0
    para: int = 0
    index: int = 0  # line number within its paragraph
    count: int = 1  # lines in the paragraph
    keep_with_next: bool = False

    @property
    def text(self) -> str:
        return " ".join(r.text for r in self.runs)


@dataclass
class Gap:
    height: int


@dataclass
class PageBreak:
    pass


@dataclass
class Page:
    lines: list[tuple[int, Line]] = field(default_factory=list)  # (y in text area, line)

    @property
    def chapter(self) -> int:
        return self.lines[0][1].chapter if self.lines else 0


# --- hyphenation -------------------------------------------------------------

# Break after these without adding a hyphen ("zwart-wit", "woei\N{EM DASH}Frits").
_BREAK_AFTER = "-\N{HYPHEN}\N{EN DASH}\N{EM DASH}"
_CORE = re.compile(r"^(\W*)(.*?)(\W*)$", re.S)


class Hyphenator:
    def __init__(self, lang: str | None, *, left: int = 2, right: int = 3, min_word: int = 5):
        code = pyphen.language_fallback(lang.replace("-", "_")) if lang else None
        self.lang = code
        self.min_word = min_word
        self._dic = pyphen.Pyphen(lang=code, left=left, right=right) if code else None

    def splits(self, word: str) -> list[tuple[str, str]]:
        """Dictionary breaks as (head + "-", tail), longest head first."""
        out: list[tuple[str, str]] = []
        lead, core, trail = _CORE.match(word).groups()
        if (
            self._dic is not None
            and len(core) >= self.min_word
            and all(c.isalpha() or c in "'\N{RIGHT SINGLE QUOTATION MARK}" for c in core)
        ):
            for head, tail in self._dic.iterate(core):
                out.append((f"{lead}{head}-", f"{tail}{trail}"))
        return out


def dash_breaks(word: str) -> list[tuple[str, str]]:
    """Breaks after a dash already in the word, as (head, tail)."""
    return [
        (word[: i + 1], word[i + 1 :])
        for i, ch in enumerate(word[:-1])
        if ch in _BREAK_AFTER and i > 0
    ]


# --- line breaking -----------------------------------------------------------


def break_lines(
    text: str,
    measure: Measurer,
    width: float,
    *,
    indent: float = 0,
    hyphenator: Hyphenator | None = None,
    justify: bool = False,
    align: str = "left",
) -> list[list[Run]]:
    """Break one paragraph into lines of positioned word runs.

    ``\\n`` in ``text`` is a forced break. The last line and lines ending in a
    forced break are never justified. Words wider than a whole line are split
    at character level as a last resort.
    """
    space = measure.width(" ")
    lines: list[list[Run]] = []

    def emit(words: list[str], offset: float, avail: float, stretch: bool) -> None:
        widths = [measure.width(w) for w in words]
        natural = sum(widths) + space * max(len(words) - 1, 0)
        gap = space
        if stretch and len(words) > 1:
            gap = space + (avail - natural) / (len(words) - 1)
        elif align == "center":
            offset += (avail - natural) / 2
        x = offset
        runs = []
        for word, w in zip(words, widths, strict=True):
            runs.append(Run(round(x, 2), word))
            x += w + gap
        lines.append(runs)

    for segment in text.split("\n"):
        queue = deque(w for w in segment.split(" ") if w)
        if not queue:
            emit([], 0, width, False)  # blank line from consecutive breaks
            continue
        current: list[str] = []
        used = 0.0
        while queue:
            offset = indent if not lines else 0
            avail = width - offset
            word = queue.popleft()
            w = measure.width(word)
            needed = w if not current else used + space + w
            if needed <= avail:
                current.append(word)
                used = needed
                continue
            room = avail - (used + space if current else 0)
            split = _fit_split(word, room, measure, hyphenator)
            if split is None and not current:
                split = _force_split(word, avail, measure)
            if split is not None:
                head, tail = split
                current.append(head)
                queue.appendleft(tail)
            else:
                queue.appendleft(word)
            emit(current, offset, avail, justify)
            current, used = [], 0.0
        emit(current, indent if not lines else 0, width - (indent if not lines else 0), False)
    return lines


def _fit_split(word, room, measure, hyphenator) -> tuple[str, str] | None:
    """Longest break of ``word`` whose head fits in ``room``."""
    if room <= 0:
        return None
    candidates = dash_breaks(word) + (hyphenator.splits(word) if hyphenator else [])
    candidates.sort(key=lambda split: len(split[0]), reverse=True)
    for head, tail in candidates:
        if measure.width(head) <= room:
            return head, tail
    return None


def _force_split(word, avail, measure) -> tuple[str, str]:
    cut = 1
    while cut < len(word) and measure.width(word[: cut + 1]) <= avail:
        cut += 1
    return word[:cut], word[cut:]


# --- book layout -------------------------------------------------------------


def layout_book(
    chapters: list[Chapter],
    measure: Measurer,
    geometry: PageGeometry,
    options: LayoutOptions,
    hyphenator: Hyphenator | None,
) -> list[Page]:
    items = book_items(chapters, measure, geometry, options, hyphenator)
    return paginate(items, geometry.text_height, options.line_height, options.widow_orphan)


def book_items(chapters, measure, geometry, options, hyphenator) -> list[Line | Gap | PageBreak]:
    width = geometry.text_width
    lh = options.line_height
    items: list[Line | Gap | PageBreak] = []
    para = 0

    def add(runs_list: list[list[Run]], chapter: int, keep: bool = False) -> None:
        nonlocal para
        for i, runs in enumerate(runs_list):
            items.append(Line(runs, chapter, para, i, len(runs_list), keep))
        para += 1

    for ci, chapter in enumerate(chapters):
        if options.chapter_page_break and items:
            items.append(PageBreak())
        prev = None
        for block in chapter.blocks:
            if block.kind == "heading":
                if prev is not None:
                    items.append(Gap(lh // 2))
                add(break_lines(block.text, measure, width, align="center"), ci, keep=True)
                items.append(Gap(lh // 4))
            elif block.kind == "text":
                if prev is not None and options.paragraph_spacing:
                    items.append(Gap(options.paragraph_spacing))
                indent = options.indent if prev == "text" else 0
                runs = break_lines(
                    block.text,
                    measure,
                    width,
                    indent=indent,
                    hyphenator=hyphenator,
                    justify=options.justify,
                )
                add(runs, ci)
            else:  # scene break or image placeholder: one centred line
                text = SCENE_BREAK if block.kind == "break" else block.text
                add(break_lines(text, measure, width, align="center"), ci)
            prev = block.kind
    return items


def paginate(
    items: list[Line | Gap | PageBreak], height: int, line_height: int, widow_orphan: bool = True
) -> list[Page]:
    pages: list[Page] = []
    page = Page()
    y = 0

    def flush() -> None:
        nonlocal page, y
        if page.lines:
            pages.append(page)
        page, y = Page(), 0

    i = 0
    while i < len(items):
        item = items[i]
        if isinstance(item, PageBreak):
            flush()
            i += 1
        elif isinstance(item, Gap):
            if page.lines:  # gaps vanish at the top of a page
                y += item.height
            i += 1
        elif y + line_height <= height or not page.lines:
            page.lines.append((y, item))
            y += line_height
            i += 1
        else:
            keep = _page_end(page.lines, item) if widow_orphan else len(page.lines)
            carried = page.lines[keep:]
            del page.lines[keep:]
            base = carried[0][0] if carried else 0
            end = y
            flush()
            page.lines = [(cy - base, line) for cy, line in carried]
            y = end - base if carried else 0
            # `item` is placed on the next loop iteration
    flush()
    return pages


def _page_end(lines: list[tuple[int, Line]], nxt: Line) -> int:
    """How many of ``lines`` stay on this page when ``nxt`` no longer fits."""
    k = len(lines)
    last = lines[k - 1][1]
    # widow: don't start the next page with the last line of a paragraph
    if nxt.index == nxt.count - 1 and nxt.count > 1 and last.para == nxt.para:
        k -= 1
    # orphan: don't end this page with the first line of a paragraph (its
    # continuation is necessarily on the next page)
    if k and lines[k - 1][1].index == 0 and lines[k - 1][1].count > 1:
        k -= 1
    # headings stay with what follows
    while k and lines[k - 1][1].keep_with_next:
        k -= 1
    return k if k > 0 else len(lines)
