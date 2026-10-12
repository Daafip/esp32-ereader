"""The .t5b book format: fixed-size 1-bpp page bitmaps plus a small header and TOC.

Layout (all integers little-endian, strings UTF-8, NUL-padded, always NUL-terminated):

    header   146 B  magic "T5B1", version u16, page_w u16, page_h u16, stride u16,
                    page_count u32, toc_count u16, title[64], author[64]
    toc      toc_count x 52 B  {start_page u32, title[48]}
    pages    page_count x (stride * page_h) B

Page n starts at ``HEADER.size + toc_count * TOC_ENTRY.size + n * page_bytes``.

Page bitmap: row-major, top row first, ``stride = ceil(page_w / 8)`` bytes per row,
MSB first (pixel x is bit ``0x80 >> (x & 7)`` of byte ``x >> 3``), 1 = white,
0 = black, row padding bits set to 1. This is bb_epaper's local framebuffer layout
for a 250x122 landscape panel, so the firmware can memcpy a page into it.
"""

from __future__ import annotations

import struct
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path

from PIL import Image

MAGIC = b"T5B1"
VERSION = 1
HEADER = struct.Struct("<4sHHHHIH64s64s")
TOC_ENTRY = struct.Struct("<I48s")
TITLE_BYTES = 64
AUTHOR_BYTES = 64
TOC_TITLE_BYTES = 48
MAX_TOC = 0xFFFF


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

    def page_offset(self, n: int) -> int:
        if not 0 <= n < self.page_count:
            raise IndexError(f"page {n} out of range 0..{self.page_count - 1}")
        return HEADER.size + self.toc_count * TOC_ENTRY.size + n * self.page_bytes


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
    magic, version, w, h, stride, count, toc_count, title, author = HEADER.unpack_from(data)
    if magic != MAGIC:
        raise ValueError(f"not a .t5b file (magic {magic!r})")
    if version != VERSION:
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


def write_t5b(
    path: Path,
    *,
    title: str,
    author: str,
    page_w: int,
    page_h: int,
    toc: list[TocEntry],
    pages: Iterable[bytes],
) -> Header:
    """Write a .t5b file. ``pages`` are already-packed bitmaps."""
    toc = toc[:MAX_TOC]
    page_bytes = stride_for(page_w) * page_h
    pages = list(pages)
    for i, page in enumerate(pages):
        if len(page) != page_bytes:
            raise ValueError(f"page {i} is {len(page)} bytes, expected {page_bytes}")
    for entry in toc:
        if not 0 <= entry.start_page < len(pages):
            raise ValueError(f"TOC entry {entry.title!r} points at page {entry.start_page}")
    header = Header(page_w, page_h, len(pages), len(toc), title, author)
    with open(path, "wb") as f:
        f.write(pack_header(header))
        for entry in toc:
            f.write(pack_toc_entry(entry))
        for page in pages:
            f.write(page)
    return header


class T5BFile:
    """Random-access reader, mirroring what the firmware does."""

    def __init__(self, path: Path):
        self.path = Path(path)
        with open(self.path, "rb") as f:
            self.header = unpack_header(f.read(HEADER.size))
            toc_data = f.read(self.header.toc_count * TOC_ENTRY.size)
        self.toc = [
            unpack_toc_entry(toc_data, i * TOC_ENTRY.size) for i in range(self.header.toc_count)
        ]
        expected = HEADER.size + len(toc_data) + self.header.page_count * self.header.page_bytes
        actual = self.path.stat().st_size
        if actual != expected:
            raise ValueError(f"file is {actual} bytes, header implies {expected}")

    def page_data(self, n: int) -> bytes:
        with open(self.path, "rb") as f:
            f.seek(self.header.page_offset(n))
            return f.read(self.header.page_bytes)

    def page_image(self, n: int) -> Image.Image:
        return unpack_page(self.page_data(n), self.header.page_w, self.header.page_h)
