import os
from pathlib import Path

import pytest
from PIL import Image, ImageChops, ImageDraw, ImageFont

from bookprep.extract import Block, Chapter
from bookprep.layout import FontMeasurer, Hyphenator, LayoutOptions, PageGeometry, layout_book
from bookprep.render import RenderOptions, render_page

GOLDEN = Path(__file__).parent / "golden" / "page_ubuntu13.png"
UPDATE = os.environ.get("BOOKPREP_UPDATE_GOLDEN") == "1"

TEXT = (
    "Ik ben makelaar in koffi, en woon op de Lauriergracht, N° 37. Het is myn gewoonte "
    "niet, romans te schryven, of zulke dingen, en het heeft dan ook lang geduurd, voor ik "
    "er toe overging een paar riem papier extra te bestellen, en het werk aantevangen."
)


def first_page(font_path, **render_kwargs):
    font = ImageFont.truetype(str(font_path), 13, layout_engine=ImageFont.Layout.BASIC)
    geometry = PageGeometry()
    chapters = [Chapter("Een", [Block("heading", "Eerste Hoofdstuk"), Block("text", TEXT)])]
    pages = layout_book(
        chapters, FontMeasurer(font), geometry, LayoutOptions(line_height=16, indent=13),
        Hyphenator("nl"),
    )  # fmt: skip
    return render_page(pages[0], font, geometry, RenderOptions(16, **render_kwargs)), geometry


def test_render_shape_and_status_band(font_path):
    img, geometry = first_page(font_path)
    assert img.mode == "1"
    assert img.size == (250, 122)
    band = img.crop((0, 122 - geometry.status_bar, 250, 122))
    assert band.getextrema() == (255, 255)  # left white for the device's status bar
    assert img.getextrema() == (0, 255)


def test_threshold_controls_weight(font_path):
    def black(img):
        return img.histogram()[0]

    light, _ = first_page(font_path, threshold=60)
    bold, _ = first_page(font_path, threshold=200)
    assert black(bold) > black(light)


def test_golden_page(font_path):
    """Pixel-exact regression test. Regenerate with BOOKPREP_UPDATE_GOLDEN=1 after an
    intended rendering change (and look at the new image before committing it)."""
    img, _ = first_page(font_path)
    if UPDATE or not GOLDEN.exists():
        GOLDEN.parent.mkdir(exist_ok=True)
        img.save(GOLDEN)
        pytest.skip(f"golden image written to {GOLDEN}")
    expected = Image.open(GOLDEN).convert("1")
    diff = ImageChops.difference(img.convert("L"), expected.convert("L"))
    assert diff.getbbox() is None, f"rendering changed in region {diff.getbbox()}"


def test_mono_measurement_matches_mono_rendering(font_path):
    """In mono mode FreeType uses hinted advances; the measurer must agree, or words
    drift into each other. Checked by where a glyph after the word lands."""
    font = ImageFont.truetype(str(font_path), 13, layout_engine=ImageFont.Layout.BASIC)
    measure = FontMeasurer(font, mode="1")

    def bar_column(x, text):
        img = Image.new("L", (200, 30), 255)
        draw = ImageDraw.Draw(img)
        draw.fontmode = "1"
        draw.text((x, 20), text, font=font, fill=0, anchor="ls")
        return img.point(lambda v: 255 - v).getbbox()[2]

    for word in ["aantevangen", "schryven", "Lauriergracht"]:
        assert bar_column(0, word + "|") == bar_column(measure.width(word), "|"), word
