from PIL import ImageFont

from bookprep.devpages import (
    STAIRCASE_BYTES,
    STAIRCASE_X,
    STAIRCASE_Y,
    calibration_image,
    main,
    sample_pages,
    sample_picture,
)
from bookprep.layout import SCREENS
from bookprep.t5b import T5BFile, pack_page


def test_calibration_page_bits(font_path):
    font = ImageFont.truetype(str(font_path), 11)
    data = pack_page(calibration_image(font))
    stride = 32
    # border: first and last row all black except the padding bits
    assert data[0] == 0x00
    assert data[121 * stride] == 0x00
    assert data[31] == 0x3F  # x=248,249 black, padding white
    # staircase: in row STAIRCASE_Y + k, pixel k of every byte is black (MSB first)
    first = STAIRCASE_X // 8
    for k in range(8):
        row = (STAIRCASE_Y + k) * stride
        for b in range(first, first + STAIRCASE_BYTES):
            assert data[row + b] == 0xFF ^ (0x80 >> k)


def test_sample_pages_are_text(font_path):
    pages = sample_pages(font_path)
    assert len(pages) == 2
    for img in pages:
        assert img.size == (250, 122)
        assert img.histogram()[0] > 500  # plenty of black pixels


def test_main_writes_header_and_t5b(tmp_path, font_path):
    header = tmp_path / "test_pages.h"
    t5b = tmp_path / "dev.t5b"
    assert main(["--header", str(header), "--t5b", str(t5b)]) == 0
    text = header.read_text(encoding="ascii")
    assert "static const uint8_t kCalibrationPage[3904]" in text
    assert "static const uint8_t kSamplePages[2][3904]" in text
    book = T5BFile(t5b)
    assert book.header.page_count == 3
    assert [e.title for e in book.toc] == ["Calibration", "Sample text"]


def test_sample_picture_is_square(tmp_path, font_path):
    for screen_id, side in (("t5-213", 122), ("4.7in", 540)):
        img = sample_picture(SCREENS[screen_id])
        assert img.mode == "1" and img.size == (side, side)
        assert 0.1 < img.histogram()[0] / side**2 < 0.9  # a picture, not a blank
    pic = tmp_path / "pic.bin"
    assert main(["--t5b", str(tmp_path / "dev.t5b"), "--picture", str(pic)]) == 0
    assert pic.stat().st_size == 16 * 122  # kSquareBytes on the T5-2.13


def test_font_sheet(sample_epub, tmp_path, font_path):
    from PIL import Image

    from bookprep.fontsheet import main as font_sheet

    out = tmp_path / "sheet.png"
    assert font_sheet([str(sample_epub), "--sizes", "11", "13", "--out", str(out)]) == 0
    assert Image.open(out).width > 1000  # two tiles side by side
