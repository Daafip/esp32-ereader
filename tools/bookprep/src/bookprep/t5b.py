"""The .t5b book format: 1-bpp page bitmaps plus a small header and TOC.

Layout (all integers little-endian, strings UTF-8, NUL-padded, always NUL-terminated):

    header   146 B  magic "T5B1", version u16, page_w u16, page_h u16, stride u16,
                    page_count u32, toc_count u16, title[64], author[64]
    toc      toc_count x 52 B  {start_page u32, title[48]}
    index    version 2: (page_count + 1) x u32, the file offset of each page's data,
             then the end of the last page
    pages    version 2: one zlib stream per page, each inflating to stride * page_h B;
             version 1: page_count x (stride * page_h) B, uncompressed
    cover    optional: "T5CV", width u16, height u16, stride u16, reserved u16, then
             the bitmap (same layout as a page, uncompressed); the file ends with it

Version 1 page n starts at ``HEADER.size + toc_count * TOC_ENTRY.size + n * page_bytes``.

Page bitmap: row-major, top row first, ``stride = ceil(page_w / 8)`` bytes per row,
MSB first (pixel x is bit ``0x80 >> (x & 7)`` of byte ``x >> 3``), 1 = white,
0 = black, row padding bits set to 1. This is bb_epaper's local framebuffer layout
for a 250x122 landscape panel, so the firmware can memcpy a page into it.
"""

from __future__ import annotations

import struct
import zlib
from collections.abc import Iterable
from dataclasses import dataclass
from itertools import pairwise
from pathlib import Path

from PIL import Image

MAGIC = b"T5B1"
VERSION = 2  # compressed pages and a page index (firmware 2.0.0 and later)
OLDEST_VERSION = 1  # uncompressed pages
HEADER = struct.Struct("<4sHHHHIH64s64s")
TOC_ENTRY = struct.Struct("<I48s")
INDEX_ENTRY = struct.Struct("<I")
ZLIB_LEVEL = 9
TITLE_BYTES = 64
AUTHOR_BYTES = 64
TOC_TITLE_BYTES = 48
MAX_TOC = 0xFFFF
COVER_MAGIC = b"T5CV"
COVER_HEADER = struct.Struct("<4sHHHH")


@dataclass(frozen=True)
class TocEntry:
    start_page: int
    title: str


@dataclass(frozen=True)
class Header:
    page_w: int
    page_h: int
    page_count: int
    toc_count: int
    title: str
    author: str
    version: int = VERSION

    @property
    def stride(self) -> int:
        return stride_for(self.page_w)

    @property
    def page_bytes(self) -> int:
        return self.stride * self.page_h

    @property
    def compressed(self) -> bool:
        return self.version >= 2

    @property
    def index_offset(self) -> int:
        return HEADER.size + self.toc_count * TOC_ENTRY.size

    @property
    def pages_offset(self) -> int:
        """Where the first page's data starts (after the index in version 2)."""
        index = (self.page_count + 1) * INDEX_ENTRY.size if self.compressed else 0
        return self.index_offset + index

    def page_offset(self, n: int) -> int:
        """Version 1 only: pages are page_bytes each."""
        if self.compressed:
            raise ValueError("compressed pages have no fixed offset (see the index)")
        if not 0 <= n < self.page_count:
            raise IndexError(f"page {n} out of range 0..{self.page_count - 1}")
        return self.pages_offset + n * self.page_bytes


def stride_for(width: int) -> int:
    return (width + 7) // 8


def encode_str(text: str, size: int) -> bytes:
    """UTF-8 encode, truncate on a character boundary to size-1 bytes, NUL-pad to size."""
    data = text.encode("utf-8")
    if len(data) > size - 1:
        data = data[: size - 1]
        while data:
            try:
                data.decode("utf-8")
                break
            except UnicodeDecodeError:
                data = data[:-1]
    return data.ljust(size, b"\0")


def part_title(title: str, first: int, last: int) -> str:
    """The title of a part of a book, "Title (301-480)" with an en dash: shortened to
    fit the 64-byte field with the range kept. bookprep.js: partTitle()."""
    suffix = f" ({first}\N{EN DASH}{last})"
    room = max(TITLE_BYTES - 1 - len(suffix.encode("utf-8")), 0)
    head = encode_str(title, room + 1).rstrip(b"\0").decode("utf-8")
    return head.rstrip() + suffix


def decode_str(data: bytes) -> str:
    return data.split(b"\0", 1)[0].decode("utf-8")


def pack_header(header: Header) -> bytes:
    return HEADER.pack(
        MAGIC,
        header.version,
        header.page_w,
        header.page_h,
        header.stride,
        header.page_count,
        header.toc_count,
        encode_str(header.title, TITLE_BYTES),
        encode_str(header.author, AUTHOR_BYTES),
    )


def unpack_header(data: bytes) -> Header:
    if len(data) < HEADER.size:
        raise ValueError("file too short")
    magic, version, w, h, stride, count, toc_count, title, author = HEADER.unpack_from(data)
    if magic != MAGIC:
        raise ValueError(f"not a .t5b file (magic {magic!r})")
    if not OLDEST_VERSION <= version <= VERSION:
        raise ValueError(f"unsupported .t5b version {version}")
    if stride != stride_for(w):
        raise ValueError(f"stride {stride} does not match width {w}")
    return Header(w, h, count, toc_count, decode_str(title), decode_str(author), version)


def pack_toc_entry(entry: TocEntry) -> bytes:
    return TOC_ENTRY.pack(entry.start_page, encode_str(entry.title, TOC_TITLE_BYTES))


def unpack_toc_entry(data: bytes, offset: int = 0) -> TocEntry:
    start, title = TOC_ENTRY.unpack_from(data, offset)
    return TocEntry(start, decode_str(title))


def pack_page(image: Image.Image) -> bytes:
    """Convert a mode-"1" image to the on-disk bitmap (padding bits = white)."""
    if image.mode != "1":
        raise ValueError(f"expected a mode '1' image, got {image.mode!r}")
    w, h = image.size
    stride = stride_for(w)
    data = bytearray(image.tobytes("raw", "1"))  # MSB first, 1 = white, rows byte-padded
    pad_bits = stride * 8 - w
    if pad_bits:
        pad_mask = (1 << pad_bits) - 1
        for row in range(h):
            data[row * stride + stride - 1] |= pad_mask
    return bytes(data)


def unpack_page(data: bytes, width: int, height: int) -> Image.Image:
    expected = stride_for(width) * height
    if len(data) != expected:
        raise ValueError(f"page is {len(data)} bytes, expected {expected}")
    return Image.frombytes("1", (width, height), data, "raw", "1")


def compress_page(data: bytes) -> bytes:
    """A packed page as stored in a version 2 file: one zlib stream."""
    return zlib.compress(data, ZLIB_LEVEL)


def pack_cover(image: Image.Image) -> bytes:
    """The optional cover block: header + bitmap of a mode-"1" image."""
    w, h = image.size
    return COVER_HEADER.pack(COVER_MAGIC, w, h, stride_for(w), 0) + pack_page(image)


def write_t5b(
    path: Path,
    *,
    title: str,
    author: str,
    page_w: int,
    page_h: int,
    toc: list[TocEntry],
    pages: Iterable[bytes],
    cover: Image.Image | None = None,
    compress: bool = True,
) -> Header:
    """Write a .t5b file. ``pages`` are already-packed bitmaps; ``cover`` a mode-"1" image.

    ``compress=False`` writes version 1 (uncompressed pages), for firmware 1.x.
    """
    toc = toc[:MAX_TOC]
    page_bytes = stride_for(page_w) * page_h
    pages = list(pages)
    for i, page in enumerate(pages):
        if len(page) != page_bytes:
            raise ValueError(f"page {i} is {len(page)} bytes, expected {page_bytes}")
    for entry in toc:
        if not 0 <= entry.start_page < len(pages):
            raise ValueError(f"TOC entry {entry.title!r} points at page {entry.start_page}")
    version = VERSION if compress else OLDEST_VERSION
    header = Header(page_w, page_h, len(pages), len(toc), title, author, version)
    if compress:
        pages = [compress_page(page) for page in pages]
    with open(path, "wb") as f:
        f.write(pack_header(header))
        for entry in toc:
            f.write(pack_toc_entry(entry))
        if compress:
            offset = header.pages_offset
            for page in pages:
                f.write(INDEX_ENTRY.pack(offset))
                offset += len(page)
            f.write(INDEX_ENTRY.pack(offset))
        for page in pages:
            f.write(page)
        if cover is not None:
            f.write(pack_cover(cover))
    return header


class T5BFile:
    """Random-access reader, mirroring what the firmware does."""

    def __init__(self, path: Path):
        self.path = Path(path)
        actual = self.path.stat().st_size
        with open(self.path, "rb") as f:
            self.header = unpack_header(f.read(HEADER.size))
            toc_data = f.read(self.header.toc_count * TOC_ENTRY.size)
            self.index: list[int] = []
            if self.header.compressed:
                count = self.header.page_count + 1
                data = f.read(count * INDEX_ENTRY.size)
                if len(data) != count * INDEX_ENTRY.size:
                    raise ValueError("file too short for its page index")
                self.index = [o for (o,) in INDEX_ENTRY.iter_unpack(data)]
        self.toc = [
            unpack_toc_entry(toc_data, i * TOC_ENTRY.size) for i in range(self.header.toc_count)
        ]
        if self.header.compressed:
            ok = self.index[0] == self.header.pages_offset and self.index[-1] <= actual
            if not ok or any(b <= a for a, b in pairwise(self.index)):
                raise ValueError("corrupt page index")
            expected = self.index[-1]
        else:
            expected = self.header.pages_offset + self.header.page_count * self.header.page_bytes
        self.cover: Image.Image | None = None
        if actual > expected:  # the optional cover block
            with open(self.path, "rb") as f:
                f.seek(expected)
                tail = f.read()
            if len(tail) >= COVER_HEADER.size:
                magic, w, h, stride, _ = COVER_HEADER.unpack_from(tail)
                well_formed = magic == COVER_MAGIC and stride == stride_for(w) and w and h
                if well_formed and len(tail) == COVER_HEADER.size + stride * h:
                    self.cover = unpack_page(tail[COVER_HEADER.size :], w, h)
            if self.cover is None:
                raise ValueError(f"file is {actual} bytes, header implies {expected} (bad cover)")
        elif actual != expected:
            raise ValueError(f"file is {actual} bytes, header implies {expected}")

    def page_span(self, n: int) -> tuple[int, int]:
        """(file offset, length) of page n as stored: a zlib stream in version 2."""
        if not 0 <= n < self.header.page_count:
            raise IndexError(f"page {n} out of range 0..{self.header.page_count - 1}")
        if self.header.compressed:
            return self.index[n], self.index[n + 1] - self.index[n]
        return self.header.page_offset(n), self.header.page_bytes

    def page_stored(self, n: int) -> bytes:
        """Page n as stored (compressed in version 2)."""
        offset, length = self.page_span(n)
        with open(self.path, "rb") as f:
            f.seek(offset)
            return f.read(length)

    def page_data(self, n: int) -> bytes:
        """Page n as a packed bitmap."""
        data = self.page_stored(n)
        if self.header.compressed:
            d = zlib.decompressobj()
            try:
                data = d.decompress(data)
            except zlib.error as e:
                raise ValueError(f"page {n}: {e}") from e
            if not d.eof or d.unused_data:  # as the firmware: exactly one stream
                raise ValueError(f"page {n}: not one complete zlib stream")
            if len(data) != self.header.page_bytes:
                raise ValueError(f"page {n} inflates to {len(data)} bytes")
        return data

    def page_image(self, n: int) -> Image.Image:
        return unpack_page(self.page_data(n), self.header.page_w, self.header.page_h)


# --- parts of a stored book (book server: a book too big for the reader) -------------


def part_toc(toc: list[TocEntry], first: int, end: int) -> list[TocEntry]:
    """The contents of pages first..end-1 of a book, numbered from the part's first
    page: the chapters that start inside it, after the one it opens in (as
    cli.toc_entries() gives for the typeset part)."""
    inside = [TocEntry(e.start_page - first, e.title) for e in toc if first <= e.start_page < end]
    before = [e for e in toc if e.start_page < first]
    if before and not (inside and inside[0].start_page == 0):
        inside.insert(0, TocEntry(0, before[-1].title))
    return inside


def part_size(book: T5BFile, first: int, end: int) -> int:
    """Bytes of the file write_part() makes for pages first..end-1."""
    if not book.header.compressed:
        raise ValueError("parts need a compressed (version 2) book")
    toc = part_toc(book.toc, first, end)
    pages = book.index[end] - book.index[first]
    cover = (
        COVER_HEADER.size + book.cover.height * stride_for(book.cover.width) if book.cover else 0
    )
    return (
        HEADER.size
        + len(toc) * TOC_ENTRY.size
        + (end - first + 1) * INDEX_ENTRY.size
        + pages
        + cover
    )


def write_part(book: T5BFile, path: Path, first: int, end: int) -> Header:
    """Pages first..end-1 of a version 2 book as a book of their own: the pages as
    stored (no new compression), its own contents, the range in the title
    ("Title (171-340)", 1-based) and the cover."""
    if not 0 <= first < end <= book.header.page_count:
        raise ValueError(f"pages {first}..{end - 1} out of range 0..{book.header.page_count - 1}")
    if not book.header.compressed:
        raise ValueError("parts need a compressed (version 2) book")
    h = book.header
    toc = part_toc(book.toc, first, end)
    title = part_title(h.title, first + 1, end) if (first, end) != (0, h.page_count) else h.title
    header = Header(h.page_w, h.page_h, end - first, len(toc), title, h.author, VERSION)
    with open(book.path, "rb") as src:
        src.seek(book.index[first])
        pages = src.read(book.index[end] - book.index[first])
    with open(path, "wb") as f:
        f.write(pack_header(header))
        for entry in toc:
            f.write(pack_toc_entry(entry))
        shift = header.pages_offset - book.index[first]
        for offset in book.index[first : end + 1]:
            f.write(INDEX_ENTRY.pack(offset + shift))
        f.write(pages)
        if book.cover is not None:
            f.write(pack_cover(book.cover))
    return header


def plan_parts(book: T5BFile, room: int) -> list[tuple[int, int]]:
    """Consecutive parts (first, end) of the whole book, each as big as fits in `room`
    bytes; [] if not even a single page fits."""
    parts = []
    first, count = 0, book.header.page_count
    while first < count:
        if part_size(book, first, first + 1) > room:
            return []
        end = first + 1
        while end < count and part_size(book, first, end + 1) <= room:
            end += 1
        parts.append((first, end))
        first = end
    return parts
