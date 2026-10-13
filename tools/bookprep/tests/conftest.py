from pathlib import Path

import pytest

from bookprep.cli import default_font


@pytest.fixture(scope="session")
def font_path() -> Path:
    path = default_font()
    if path is None:
        pytest.skip("Ubuntu-R.ttf not found; run the tests through pixi")
    return path


class MonoMeasurer:
    """Every character is 6 px wide, so expected layouts can be worked out by hand."""

    def width(self, text: str) -> float:
        return 6.0 * len(text)


@pytest.fixture
def mono() -> MonoMeasurer:
    return MonoMeasurer()


def make_epub(path: Path) -> Path:
    """Small EPUB exercising the extractor: TOC fragment, continuation file, br, hr, img."""
    from ebooklib import epub

    book = epub.EpubBook()
    book.set_identifier("bookprep-test")
    book.set_title("Testboek")
    book.set_language("nl")
    book.add_author("A. Schrijver")

    cover = epub.EpubHtml(title="Cover", file_name="cover.xhtml", lang="nl")
    cover.content = '<html><body><img src="cover.jpg" alt="cover"/></body></html>'
    ch1 = epub.EpubHtml(title="Een", file_name="ch1.xhtml", lang="nl")
    ch1.content = (
        "<html><body>"
        "<h1>Hoofdstuk 1 [3]</h1>"
        "<p>Eerste   alinea\n met  witruimte.</p>"
        "<div><p>Regel een<br/>regel twee</p></div>"
        "<hr/>"
        "<p>Voor het plaatje.</p><p><img src='x.png'/></p>"
        "<h2 id='c2'>Hoofdstuk 2</h2>"
        "<p>Tweede hoofdstuk begint hier.</p>"
        "</body></html>"
    )
    ch1b = epub.EpubHtml(title="Vervolg", file_name="ch1b.xhtml", lang="nl")
    ch1b.content = "<html><body><p>Vervolg van hoofdstuk twee.</p></body></html>"
    for item in (cover, ch1, ch1b):
        book.add_item(item)
    book.toc = [
        epub.Link("ch1.xhtml", "Hoofdstuk 1", "h1"),
        epub.Link("ch1.xhtml#c2", "Hoofdstuk 2", "h2"),
    ]
    book.add_item(epub.EpubNcx())
    book.add_item(epub.EpubNav())
    book.spine = [cover, ch1, ch1b]
    book.set_cover("cover.png", cover_png(), create_page=False)
    epub.write_epub(str(path), book)
    return path


def cover_png() -> bytes:
    """A small cover image: a dark band across a light background."""
    import io

    from PIL import Image, ImageDraw

    img = Image.new("RGB", (60, 90), (230, 220, 200))
    ImageDraw.Draw(img).rectangle((0, 30, 59, 59), fill=(30, 30, 60))
    out = io.BytesIO()
    img.save(out, "PNG")
    return out.getvalue()


@pytest.fixture
def sample_epub(tmp_path) -> Path:
    return make_epub(tmp_path / "testboek.epub")
