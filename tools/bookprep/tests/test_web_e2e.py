"""bookprep.js in a real browser (headless Chromium): EPUB parsing with DOMParser,
font loading, canvas rendering, .t5b output. Compared with Python bookprep."""

import base64
import functools
import html
import json
import re
import shutil
import subprocess
import threading
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest

from bookprep.cli import clean_book, toc_entries
from bookprep.extract import extract
from bookprep.layout import FontMeasurer, Hyphenator, LayoutOptions, PageGeometry, layout_book
from bookprep.t5b import T5BFile
from bookprep.webassets import export_hyphenation

ROOT = Path(__file__).resolve().parents[3]
WEB = ROOT / "firmware" / "web"


def chromium() -> str | None:
    for name in ("chromium", "chromium-browser", "google-chrome", "chrome"):
        if path := shutil.which(name):
            return path
    return None


pytestmark = pytest.mark.skipif(chromium() is None, reason="needs Chromium/Chrome")


class Quiet(SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


def run_in_browser(serve_dir: Path) -> dict:
    handler = functools.partial(Quiet, directory=str(serve_dir))
    server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        url = f"http://127.0.0.1:{server.server_address[1]}/e2e.html"
        dom = subprocess.run(
            [chromium(), "--headless=new", "--disable-gpu", "--no-first-run",
             "--virtual-time-budget=120000", "--dump-dom", url],
            capture_output=True, text=True, timeout=300, check=True,
        ).stdout  # fmt: skip
    finally:
        server.shutdown()
    match = re.search(r'<pre id="result">(.*?)</pre>', dom, re.S)
    assert match, dom[:500]
    result = json.loads(html.unescape(match.group(1)))
    assert "error" not in result, result["error"]
    return result


def serve_dir(tmp_path: Path, book: Path, font: Path, settings: dict, with_raw: bool) -> Path:
    d = tmp_path / "www"
    d.mkdir()
    shutil.copy(WEB / "bookprep.js", d)
    shutil.copy(WEB / "test" / "e2e.html", d)
    shutil.copy(book, d / book.name)
    shutil.copy(font, d / "font.ttf")
    (d / "hyph.txt").write_text(export_hyphenation("nl_NL"), encoding="utf-8")
    opts = {"file": book.name, "settings": settings, "withRaw": with_raw}
    (d / "options.json").write_text(json.dumps(opts), encoding="utf-8")
    return d


def test_browser_conversion_matches_python(tmp_path, sample_epub, font_path):
    result = run_in_browser(serve_dir(tmp_path, sample_epub, font_path, {}, True))

    want = extract(sample_epub)
    raw = result["raw"]
    assert (raw["title"], raw["author"], raw["language"]) == (
        want.title,
        want.author,
        want.language,
    )
    assert [c["title"] for c in raw["chapters"]] == [c.title for c in want.chapters]
    assert [[(b["kind"], b["text"]) for b in c["blocks"]] for c in raw["chapters"]] == [
        [(b.kind, b.text) for b in c.blocks] for c in want.chapters
    ]

    path = tmp_path / "out.t5b"
    path.write_bytes(base64.b64decode(result["t5b"]))
    book = T5BFile(path)  # validates sizes
    assert (book.header.page_w, book.header.page_h, book.header.stride) == (250, 122, 32)
    assert book.header.title == "Testboek"

    # same TOC as the Python tool laying out the same book with the same font
    from PIL import ImageFont

    cleaned = clean_book(want, lambda t: t)
    font = ImageFont.truetype(str(font_path), 13, layout_engine=ImageFont.Layout.BASIC)
    pages = layout_book(cleaned.chapters, FontMeasurer(font), PageGeometry(),
                        LayoutOptions(line_height=16, indent=13), Hyphenator("nl"))  # fmt: skip
    assert [(e.start_page, e.title) for e in book.toc] == [
        (e.start_page, e.title) for e in toc_entries(cleaned.chapters, pages)
    ]
    assert book.header.page_count == len(pages)

    first = book.page_image(0)
    assert first.histogram()[0] > 200  # text was rendered
    band = first.crop((0, 122 - 14, 250, 122))
    assert band.getextrema() == (255, 255)  # status bar rows left white


def test_browser_settings_change_layout(tmp_path, sample_epub, font_path):
    big = run_in_browser(serve_dir(tmp_path, sample_epub, font_path,
                                   {"size": 18, "margin": 8, "justify": False}, False))  # fmt: skip
    assert big["pages"] >= 3


@pytest.mark.parametrize(
    ("screen", "size"), [("t5-213", (250, 122, 13)), ("4.7in", (540, 960, 28))]
)
def test_page_typesets_and_uploads_to_the_reader_api(tmp_path, sample_epub, screen, size):
    """The real page: open an EPUB, typeset it for the reader's screen (from
    /api/info), upload book + original + settings to the dev server (same API as
    the firmware)."""
    from bookprep.devserver import make_server

    srv = make_server(tmp_path, "1234", port=0, fonts=False, screen=screen)
    shutil.copy(sample_epub, tmp_path / "www" / "testboek.epub")
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        url = f"http://127.0.0.1:{srv.server_address[1]}/?k=1234#typeset=testboek.epub&upload"
        dom = subprocess.run(
            [chromium(), "--headless=new", "--disable-gpu", "--no-first-run",
             "--virtual-time-budget=60000", "--dump-dom", url],
            capture_output=True, text=True, timeout=300, check=True,
        ).stdout  # fmt: skip
    finally:
        srv.shutdown()
    status = re.search(r'<body[^>]*data-upload="([^"]*)"', dom)
    assert status and status.group(1) == "ok", status.group(1) if status else dom[-500:]
    sd = tmp_path / "sd" / "books"
    book = T5BFile(sd / "testboek.t5b")
    assert book.header.title == "Testboek"
    assert (book.header.page_w, book.header.page_h) == size[:2]
    assert [e.title for e in book.toc] == ["Hoofdstuk 1", "Hoofdstuk 2"]
    assert (sd / ".src" / "testboek.epub").read_bytes() == sample_epub.read_bytes()
    settings = json.loads((sd / ".src" / "testboek.json").read_text())
    assert (settings["size"], settings["screen"]) == (size[2], screen)
