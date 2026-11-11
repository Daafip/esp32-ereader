import itertools
import struct
import zlib

import pytest
from PIL import Image

from bookprep.t5b import (
    HEADER,
    INDEX_ENTRY,
    TOC_ENTRY,
    Header,
    T5BFile,
    TocEntry,
    encode_str,
    pack_header,
    pack_page,
    part_title,
    unpack_header,
    unpack_page,
    write_t5b,
)


def white_page(w=250, h=122):
    return Image.new("1", (w, h), 1)


def test_struct_sizes():
    assert HEADER.size == 146
    assert TOC_ENTRY.size == 52


def test_header_roundtrip():
    header = Header(250, 122, 400, 12, "Max Havelaar — één", "Multatuli")
    assert unpack_header(pack_header(header)) == header
    assert header.stride == 32
    assert header.page_bytes == 3904


def test_encode_str_truncates_on_char_boundary_and_terminates():
    data = encode_str("é" * 40, 64)  # 80 bytes of UTF-8
    assert len(data) == 64
    assert data[-1] == 0
    assert data.rstrip(b"\0").decode("utf-8") == "é" * 31


def test_page_offset_math():
    header = Header(250, 122, 10, 3, "t", "a", version=1)
    first = HEADER.size + 3 * TOC_ENTRY.size
    assert header.page_offset(0) == first == 302
    assert header.page_offset(5) == first + 5 * 3904
    with pytest.raises(IndexError):
        header.page_offset(10)
    v2 = Header(250, 122, 10, 3, "t", "a")
    assert v2.compressed and v2.index_offset == first
    assert v2.pages_offset == first + 11 * 4
    with pytest.raises(ValueError):
        v2.page_offset(0)


def test_pack_page_bit_layout():
    img = white_page()
    img.putpixel((0, 0), 0)  # byte 0, bit 0x80
    img.putpixel((9, 1), 0)  # row 1, byte 1, bit 0x40
    img.putpixel((249, 0), 0)  # byte 31, bit 0x40 (249 & 7 == 1)
    data = pack_page(img)
    assert len(data) == 3904
    assert data[0] == 0x7F
    assert data[32 + 1] == 0xBF
    assert data[31] == 0xBF  # pixel 249 black, padding bits (0x3F) stay white
    assert data[32 + 31] == 0xFF  # white row: padding bits are 1
    assert set(data[2:31]) == {0xFF}


def test_pack_unpack_roundtrip():
    img = white_page()
    for x in range(0, 250, 7):
        img.putpixel((x, x % 122), 0)
    back = unpack_page(pack_page(img), 250, 122)
    assert back.tobytes() == img.tobytes()


def test_pack_page_rejects_non_1bit():
    with pytest.raises(ValueError):
        pack_page(Image.new("L", (250, 122), 255))


def three_pages():
    pages = []
    for n in range(3):
        img = white_page()
        img.putpixel((n, n), 0)
        pages.append(pack_page(img))
    return pages


@pytest.mark.parametrize("compress", [True, False])
def test_write_and_read_file(tmp_path, compress):
    pages = three_pages()
    toc = [TocEntry(0, "Begin"), TocEntry(2, "Hoofdstuk twee")]
    path = tmp_path / "b.t5b"
    header = write_t5b(path, title="T", author="A", page_w=250, page_h=122, toc=toc, pages=pages,
                       compress=compress)  # fmt: skip

    book = T5BFile(path)
    assert book.header == header
    assert book.header.version == (2 if compress else 1)
    assert book.header.page_count == 3
    assert book.header.toc_count == 2
    assert book.toc == toc
    assert [book.page_data(n) for n in range(3)] == pages
    assert book.page_image(1).getpixel((1, 1)) == 0
    raw_size = HEADER.size + 2 * TOC_ENTRY.size + 3 * 3904
    if compress:
        assert path.stat().st_size < raw_size / 10
    else:
        assert path.stat().st_size == raw_size


def test_compressed_layout(tmp_path):
    """Index right after the TOC; each page a zlib stream; the cover after the last."""
    pages = three_pages()
    cover = Image.new("1", (13, 4), 1)
    path = tmp_path / "b.t5b"
    write_t5b(path, title="T", author="A", page_w=250, page_h=122, toc=[TocEntry(0, "x")],
              pages=pages, cover=cover)  # fmt: skip
    data = path.read_bytes()
    first = HEADER.size + TOC_ENTRY.size
    index = [o for (o,) in INDEX_ENTRY.iter_unpack(data[first : first + 4 * 4])]
    assert index[0] == first + 4 * 4
    for n, page in enumerate(pages):
        assert zlib.decompress(data[index[n] : index[n + 1]]) == page
    assert data[index[3] : index[3] + 4] == b"T5CV"
    book = T5BFile(path)
    assert book.page_stored(1) == data[index[1] : index[2]]
    assert book.cover is not None


def test_corrupt_index_and_page_rejected(tmp_path):
    path = tmp_path / "b.t5b"
    write_t5b(path, title="T", author="A", page_w=250, page_h=122, toc=[], pages=three_pages())
    good = path.read_bytes()
    first = HEADER.size

    def patched(entry, value):
        data = bytearray(good)
        struct.pack_into("<I", data, first + 4 * entry, value)
        path.write_bytes(bytes(data))

    start = struct.unpack_from("<I", good, first)[0]
    for entry, value in [(0, start + 1), (3, len(good) + 1), (1, start)]:
        patched(entry, value)
        with pytest.raises(ValueError, match="index"):
            T5BFile(path)

    data = bytearray(good)
    data[start + 5] ^= 0xFF  # inside page 0's deflate data
    path.write_bytes(bytes(data))
    book = T5BFile(path)  # the index is fine
    with pytest.raises(ValueError, match="page 0"):
        book.page_data(0)
    assert book.page_data(1) == three_pages()[1]


def test_part_title_keeps_the_range():
    assert part_title("Max Havelaar", 301, 480) == "Max Havelaar (301\N{EN DASH}480)"
    long = part_title("\N{LATIN SMALL LETTER E WITH ACUTE}" * 40, 1, 170)  # 80 bytes of title
    assert long.endswith(" (1\N{EN DASH}170)")
    assert len(long.encode("utf-8")) <= 63  # fits the field with its NUL
    assert encode_str(long, 64).rstrip(b"\0").decode("utf-8") == long  # nothing cut off
    assert part_title("Een lange titel " * 4, 1, 2).count("  ") == 0  # no space before "("


def test_unknown_version_rejected():
    data = bytearray(pack_header(Header(250, 122, 1, 0, "t", "a")))
    struct.pack_into("<H", data, 4, 3)
    with pytest.raises(ValueError, match="version 3"):
        unpack_header(bytes(data))


def test_write_rejects_toc_outside_book(tmp_path):
    with pytest.raises(ValueError):
        write_t5b(
            tmp_path / "x.t5b",
            title="T",
            author="A",
            page_w=250,
            page_h=122,
            toc=[TocEntry(5, "x")],
            pages=[pack_page(white_page())],
        )


def test_bad_magic_rejected():
    data = bytearray(pack_header(Header(250, 122, 1, 0, "t", "a")))
    data[:4] = b"NOPE"
    with pytest.raises(ValueError):
        unpack_header(bytes(data))


def test_short_file_rejected(tmp_path):
    path = tmp_path / "short.t5b"
    path.write_bytes(b"T5B1 short")
    with pytest.raises(ValueError, match="too short"):
        T5BFile(path)


# --- parts of a stored book (book server) ----------------------------------------------


def _book_with_chapters(tmp_path, count=12, cover=True):
    """A version 2 book: pages that compress differently, chapters at 0, 3, 7."""
    from PIL import ImageDraw

    from bookprep.t5b import TocEntry, pack_page, write_t5b

    pages = []
    for n in range(count):
        img = Image.new("1", (250, 122), 1)
        ImageDraw.Draw(img).rectangle((0, 0, 20 * n, 10 + 9 * n), fill=0)
        pages.append(pack_page(img))
    toc = [TocEntry(0, "Een"), TocEntry(3, "Twee"), TocEntry(7, "Drie")]
    path = tmp_path / "book.t5b"
    write_t5b(path, title="Boek", author="A", page_w=250, page_h=122, toc=toc, pages=pages,
              cover=Image.new("1", (40, 60), 0) if cover else None)  # fmt: skip
    return T5BFile(path)


def test_part_toc_carries_the_chapter_it_opens_in():
    from bookprep.t5b import TocEntry, part_toc

    toc = [TocEntry(0, "Een"), TocEntry(3, "Twee"), TocEntry(7, "Drie")]
    assert part_toc(toc, 0, 12) == toc
    assert part_toc(toc, 4, 8) == [TocEntry(0, "Twee"), TocEntry(3, "Drie")]
    assert part_toc(toc, 3, 5) == [TocEntry(0, "Twee")]  # starts on a chapter
    assert part_toc(toc, 8, 12) == [TocEntry(0, "Drie")]
    assert part_toc([TocEntry(2, "Later")], 0, 2) == []  # before the first chapter


def test_write_part_keeps_pages_as_stored_with_its_own_title_and_cover(tmp_path):
    from bookprep.t5b import part_size, write_part

    book = _book_with_chapters(tmp_path)
    out = tmp_path / "part.t5b"
    header = write_part(book, out, 4, 9)
    part = T5BFile(out)
    assert header.title == "Boek (5\N{EN DASH}9)" == part.header.title
    assert part.header.page_count == 5
    assert [part.page_stored(n) for n in range(5)] == [book.page_stored(4 + n) for n in range(5)]
    assert [(e.start_page, e.title) for e in part.toc] == [(0, "Twee"), (3, "Drie")]
    assert part.cover is not None and part.cover.size == (40, 60)
    assert out.stat().st_size == part_size(book, 4, 9)
    whole = tmp_path / "whole.t5b"
    write_part(book, whole, 0, 12)  # the whole book keeps its title
    assert T5BFile(whole).header.title == "Boek"
    assert whole.read_bytes() == book.path.read_bytes()


def test_plan_parts_covers_the_book_in_pieces_that_fit(tmp_path):
    from bookprep.t5b import part_size, plan_parts

    for cover in (True, False):
        book = _book_with_chapters(tmp_path, cover=cover)
        whole = book.path.stat().st_size
        assert plan_parts(book, whole) == [(0, 12)]
        one_page = max(part_size(book, n, n + 1) for n in range(12))  # one a part
        for room in (whole - 1, (whole + one_page) // 2, one_page):
            parts = plan_parts(book, room)
            assert parts[0][0] == 0 and parts[-1][1] == 12
            assert all(a[1] == b[0] for a, b in itertools.pairwise(parts))
            assert all(part_size(book, a, b) <= room for a, b in parts)
            # each part as big as fits: one more page would not
            assert all(part_size(book, a, b + 1) > room for a, b in parts[:-1])
        assert plan_parts(book, 100) == []  # not even one page
