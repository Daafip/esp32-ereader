"""Turn a DRM-free EPUB or a plain-text file into chapters of blocks.

v1 keeps only text structure: paragraphs, headings, forced line breaks (``\\n``
inside a paragraph), scene breaks (``<hr>``) and an ``[image]`` placeholder.
Inline styling (italic, bold) is dropped.
"""

from __future__ import annotations

import re
import warnings
from dataclasses import dataclass, field
from pathlib import Path
from posixpath import normpath
from typing import Literal
from urllib.parse import unquote

from bs4 import BeautifulSoup, Comment, NavigableString, Tag, XMLParsedAsHTMLWarning

# "anchor" is internal to extract_epub: a TOC target inside a document.
BlockKind = Literal["text", "heading", "break", "image", "anchor"]


@dataclass
class Block:
    kind: BlockKind
    text: str = ""


@dataclass
class Chapter:
    title: str | None
    blocks: list[Block] = field(default_factory=list)


@dataclass
class Book:
    title: str
    author: str
    language: str | None
    chapters: list[Chapter]
    cover: bytes | None = None  # the EPUB's cover image file, if it has one


HEADING_TAGS = {"h1", "h2", "h3", "h4", "h5", "h6"}
IMAGE_TAGS = {"img", "svg", "image"}
SKIP_TAGS = {"head", "script", "style", "title", "meta", "link", "noscript"}
BLOCK_TAGS = {
    "address", "article", "aside", "blockquote", "body", "dd", "div", "dl", "dt",
    "figcaption", "figure", "footer", "header", "li", "main", "nav", "ol", "p",
    "pre", "section", "table", "tbody", "td", "tfoot", "th", "thead", "tr", "ul",
}  # fmt: skip

LINE_BREAK = "\u2028"  # placeholder for <br> while whitespace is collapsed
_WS = re.compile(r"[ \t\r\n\f]+")


def extract(path: Path) -> Book:
    path = Path(path)
    suffix = path.suffix.lower()
    if suffix == ".epub":
        return extract_epub(path)
    if suffix == ".txt":
        return extract_txt(path)
    raise ValueError(f"unsupported input {path.name!r}: expected .epub or .txt")


# --- EPUB --------------------------------------------------------------------


def extract_epub(path: Path) -> Book:
    import ebooklib
    from ebooklib import epub

    with warnings.catch_warnings():
        warnings.simplefilter("ignore")  # ebooklib warns about future option defaults
        book = epub.read_epub(str(path), options={"ignore_ncx": False})

    def meta(name: str) -> list[str]:
        return [value for value, _attrs in book.get_metadata("DC", name) if value]

    targets = _toc_targets(book.toc)
    doc_titles: dict[str, str] = {}
    anchors: dict[str, dict[str, str]] = {}
    for doc, fragment, title in targets:
        if fragment is None:
            doc_titles.setdefault(doc, title)
        else:
            anchors.setdefault(doc, {}).setdefault(fragment, title)

    # Chapters start where the TOC points: at a document or at an anchor inside
    # one. Documents the TOC doesn't point at continue the previous chapter
    # (e.g. Gutenberg splits long chapters into several files). Without a TOC,
    # every document is a chapter titled by its first heading.
    chapters: list[Chapter] = []
    for idref, linear in book.spine:
        if linear == "no":
            continue
        item = book.get_item_with_id(idref)
        if item is None or item.get_type() != ebooklib.ITEM_DOCUMENT:
            continue
        name = normpath(item.get_name())
        blocks = html_to_blocks(item.get_content(), anchors.get(name, {}))
        if not any(b.kind in ("text", "heading") for b in blocks):
            continue  # cover pages, image-only pages, empty documents
        if name in doc_titles or not chapters or not targets:
            title = doc_titles.get(name)
            if title is None and not targets:
                title = next((b.text for b in blocks if b.kind == "heading"), None)
            chapters.append(Chapter(title))
        for block in blocks:
            if block.kind == "anchor":
                chapters.append(Chapter(block.text))
            else:
                chapters[-1].blocks.append(block)
    chapters = [c for c in chapters if any(b.kind in ("text", "heading") for b in c.blocks)]

    titles = meta("title")
    return Book(
        title=titles[0] if titles else path.stem,
        author=", ".join(meta("creator")),
        language=(meta("language") or [None])[0],
        chapters=chapters,
        cover=_cover_image(book),
    )


def _cover_image(book) -> bytes | None:
    """The cover image: EPUB 3 "cover-image" item, EPUB 2 <meta name="cover">, or
    an image called cover."""
    import ebooklib

    for item in book.get_items_of_type(ebooklib.ITEM_COVER):
        return item.get_content()
    try:
        cover_meta = book.get_metadata("OPF", "cover")
    except KeyError:  # ebooklib raises when the OPF has no such meta
        cover_meta = []
    for _value, attrs in cover_meta:
        item = book.get_item_with_id((attrs or {}).get("content", ""))
        if item is not None and item.get_type() in (ebooklib.ITEM_IMAGE, ebooklib.ITEM_COVER):
            return item.get_content()
    for item in book.get_items_of_type(ebooklib.ITEM_IMAGE):
        if "cover" in item.get_name().lower() or "cover" in (item.get_id() or "").lower():
            return item.get_content()
    return None


_FOOTNOTE_REF = re.compile(r"\s*\[\d+\]$")


def _toc_targets(toc) -> list[tuple[str, str | None, str]]:
    """Flatten the EPUB TOC to (document href, fragment or None, title), in order."""
    targets: list[tuple[str, str | None, str]] = []

    def visit(entries) -> None:
        for entry in entries:
            if isinstance(entry, tuple):  # (Section, children)
                section, children = entry
                add(section)
                visit(children)
            else:
                add(entry)

    def add(entry) -> None:
        href = getattr(entry, "href", None)
        title = collapse(getattr(entry, "title", None) or "")
        title = _FOOTNOTE_REF.sub("", title)
        if href and title:
            doc, _, fragment = unquote(href).partition("#")
            targets.append((normpath(doc), fragment or None, title))

    visit(toc)
    return targets


def html_to_blocks(content: bytes | str, anchors: dict[str, str] | None = None) -> list[Block]:
    """Blocks of one (X)HTML document.

    ``anchors`` maps element ids to TOC titles; an "anchor" block is emitted
    where such an element starts.
    """
    anchors = dict(anchors or {})
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", XMLParsedAsHTMLWarning)  # XHTML parses fine as HTML
        soup = BeautifulSoup(content, "lxml")
    root = soup.body or soup
    blocks: list[Block] = []
    buffer: list[str] = []

    def flush() -> None:
        text = clean_inline("".join(buffer))
        buffer.clear()
        if text:
            blocks.append(Block("text", text))

    def walk(node: Tag) -> None:
        for child in node.children:
            if isinstance(child, Comment):
                continue
            if isinstance(child, NavigableString):
                buffer.append(str(child))
                continue
            if not isinstance(child, Tag):
                continue
            name = child.name.lower().split(":")[-1]
            if name in SKIP_TAGS:
                continue
            for key in (child.get("id"), child.get("name")):
                if key in anchors:
                    flush()
                    blocks.append(Block("anchor", anchors.pop(key)))
            if name == "br":
                buffer.append(LINE_BREAK)
            elif name == "hr":
                flush()
                blocks.append(Block("break"))
            elif name in IMAGE_TAGS:
                flush()
                blocks.append(Block("image", "[image]"))
            elif name in HEADING_TAGS:
                flush()
                text = clean_inline(child.get_text(separator=" ")).replace("\n", " ")
                text = _FOOTNOTE_REF.sub("", text)
                if text:
                    blocks.append(Block("heading", text))
            elif name in BLOCK_TAGS:
                flush()
                walk(child)
                flush()
            else:
                walk(child)

    walk(root)
    flush()
    return blocks


def collapse(text: str) -> str:
    return _WS.sub(" ", text).strip()


def clean_inline(text: str) -> str:
    """Collapse HTML whitespace; turn <br> placeholders into newlines."""
    text = collapse(text)
    lines = [line.strip() for line in text.split(LINE_BREAK)]
    return "\n".join(lines).strip("\n")


# --- plain text --------------------------------------------------------------

_GUTENBERG_START = re.compile(r"^\*\*\* ?START OF (THE|THIS) PROJECT GUTENBERG.*$", re.I | re.M)
_GUTENBERG_END = re.compile(r"^\*\*\* ?END OF (THE|THIS) PROJECT GUTENBERG.*$", re.I | re.M)
_CHAPTER_HEADING = re.compile(
    r"^(chapter|hoofdstuk|part|deel|book|boek|prologue|proloog|epilogue|epiloog)\b.{0,60}$"
    r"|^(?-i:[IVXLC]{1,7})\.?$",  # roman numerals: upper case only ("civil" is no heading)
    re.I,
)


def extract_txt(path: Path) -> Book:
    raw = Path(path).read_bytes()
    try:
        text = raw.decode("utf-8-sig")
    except UnicodeDecodeError:
        text = raw.decode("cp1252", errors="replace")
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    if start := _GUTENBERG_START.search(text):
        text = text[start.end() :]
    if end := _GUTENBERG_END.search(text):
        text = text[: end.start()]

    chapters: list[Chapter] = [Chapter(None)]
    for para in re.split(r"\n\s*\n", text):
        para = collapse(para)
        if not para:
            continue
        if _CHAPTER_HEADING.match(para):
            chapters.append(Chapter(para, [Block("heading", para)]))
        else:
            chapters[-1].blocks.append(Block("text", para))
    chapters = [c for c in chapters if c.blocks]
    return Book(title=Path(path).stem, author="", language=None, chapters=chapters)
