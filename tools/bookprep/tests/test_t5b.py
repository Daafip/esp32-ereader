import pytest
from PIL import Image

from bookprep.t5b import (
    HEADER,
    TOC_ENTRY,
    Header,
    T5BFile,
    TocEntry,
    encode_str,
    pack_header,
    pack_page,
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
    header = Header(250, 122, 10, 3, "t", "a")
    first = HEADER.size + 3 * TOC_ENTRY.size
    assert header.page_offset(0) == first == 302
    assert header.page_offset(5) == first + 5 * 3904
    with pytest.raises(IndexError):
        header.page_offset(10)


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


def test_write_and_read_file(tmp_path):
    pages = []
    for n in range(3):
        img = white_page()
        img.putpixel((n, n), 0)
        pages.append(pack_page(img))
    toc = [TocEntry(0, "Begin"), TocEntry(2, "Hoofdstuk twee")]
    path = tmp_path / "b.t5b"
    write_t5b(path, title="T", author="A", page_w=250, page_h=122, toc=toc, pages=pages)

    book = T5BFile(path)
    assert book.header.page_count == 3
    assert book.header.toc_count == 2
    assert book.toc == toc
    assert [book.page_data(n) for n in range(3)] == pages
    assert path.stat().st_size == HEADER.size + 2 * TOC_ENTRY.size + 3 * 3904
    assert book.page_image(1).getpixel((1, 1)) == 0


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
