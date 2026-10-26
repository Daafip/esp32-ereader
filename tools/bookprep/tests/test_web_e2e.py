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
from bookprep.layout import (
    SCREENS,
    FontMeasurer,
    Hyphenator,
    LayoutOptions,
    PageGeometry,
    layout_book,
)
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


def upload_from_page(tmp_path: Path, epub: Path, name: str, **server) -> tuple[str, str]:
    """The real page against the dev server: open `epub` under file name `name`,
    typeset it and press "Upload to reader". Returns (body data-upload, the DOM)."""
    from urllib.parse import quote

    from bookprep.devserver import make_server

    srv = make_server(tmp_path, "1234", port=0, fonts=False, update_site="/updates/", **server)
    shutil.copy(epub, tmp_path / "www" / name)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        url = f"http://127.0.0.1:{srv.server_address[1]}/?k=1234#typeset={quote(name)}&upload"
        dom = subprocess.run(
            [chromium(), "--headless=new", "--disable-gpu", "--no-first-run",
             "--virtual-time-budget=60000", "--dump-dom", url],
            capture_output=True, text=True, timeout=300, check=True,
        ).stdout  # fmt: skip
    finally:
        srv.shutdown()
    status = re.search(r'<body[^>]*data-upload="([^"]*)"', dom)
    assert status, dom[-500:]
    return html.unescape(status.group(1)), dom


@pytest.mark.parametrize(
    ("screen", "size"),
    [
        ("t5-213", (250, 122, 13)),
        ("4.7in", (540, 960, 28)),
        ("7.5in", (480, 800, 18)),
        ("7.5in-landscape", (800, 480, 22)),
    ],
)
def test_page_typesets_and_uploads_to_the_reader_api(tmp_path, sample_epub, screen, size):
    """The real page: open an EPUB, typeset it for the reader's screen (from
    /api/info), upload book + original + settings to the dev server (same API as
    the firmware)."""
    status, _ = upload_from_page(tmp_path, sample_epub, "testboek.epub", screen=screen)
    assert status == "ok", status
    sd = tmp_path / "sd" / "books"
    book = T5BFile(sd / "testboek.t5b")
    assert book.header.title == "Testboek"
    assert (book.header.page_w, book.header.page_h) == size[:2]
    assert [e.title for e in book.toc] == ["Hoofdstuk 1", "Hoofdstuk 2"]
    assert (sd / ".src" / "testboek.epub").read_bytes() == sample_epub.read_bytes()
    settings = json.loads((sd / ".src" / "testboek.json").read_text())
    assert (settings["size"], settings["screen"]) == (size[2], screen)
    # the EPUB's cover came along, scaled into the screen's cover box
    box = (SCREENS[screen].cover_w, SCREENS[screen].cover_h)
    assert book.cover is not None
    assert book.cover.width <= box[0] and book.cover.height <= box[1]


def test_page_shortens_long_file_names(tmp_path, sample_epub):
    """The reader takes names of 64 bytes at most (the book, its original and its
    settings); longer ones were refused as "file type not allowed here"."""
    name = "Je hebt wel iets te verbergen - Maurits Martijn, Dimitri Tokmetzis.epub"
    status, _ = upload_from_page(tmp_path, sample_epub, name)
    assert status == "ok", status
    sd = tmp_path / "sd" / "books"
    stem = "Je_hebt_wel_iets_te_verbergen_-_Maurits_Martijn_Dimitri_Tok"
    for path in (sd / f"{stem}.t5b", sd / ".src" / f"{stem}.epub", sd / ".src" / f"{stem}.json"):
        assert path.exists() and len(path.name) <= 64, path


def test_page_refuses_a_book_that_does_not_fit_before_sending(tmp_path, sample_epub):
    """A full card: the page says so at once instead of after sending everything."""
    status, dom = upload_from_page(tmp_path, sample_epub, "testboek.epub", card_mb=0.002)
    assert status.startswith("error: Not enough space on the reader"), status
    assert "2 KB free of 2 KB" in dom  # the storage line
    assert not list((tmp_path / "sd").rglob("*.t5b"))


def test_page_searches_a_book_on_the_reader(tmp_path, sample_epub, font_path):
    """The book sheet: page bitmaps from the reader, search in the original."""
    from bookprep.cli import main as prep
    from bookprep.devserver import make_server

    srv = make_server(tmp_path, "1234", port=0, fonts=True, update_site="/updates/")
    sd = tmp_path / "sd" / "books"
    (sd / ".src").mkdir(parents=True)
    assert prep([str(sample_epub), "--out", str(sd)]) == 0
    shutil.copy(sample_epub, sd / ".src" / "testboek.epub")
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        port = srv.server_address[1]
        url = f"http://127.0.0.1:{port}/?k=1234#reader=testboek.t5b&find=alinea"
        dom = subprocess.run(
            [chromium(), "--headless=new", "--disable-gpu", "--no-first-run",
             "--virtual-time-budget=60000", "--dump-dom", url],
            capture_output=True, text=True, timeout=300, check=True,
        ).stdout  # fmt: skip
    finally:
        srv.shutdown()
    hits = re.search(r'<body[^>]*data-hits="(\d+)"', dom)
    assert hits and int(hits.group(1)) >= 1, dom[-800:]
    assert "<mark>alinea</mark>" in dom


def test_page_shows_a_book_for_the_other_orientation(tmp_path, sample_epub):
    """The 7.5" turns: on a portrait reader a landscape book is marked as such, its
    pages are drawn landscape, and search typesets the original for landscape."""
    from bookprep.cli import main as prep
    from bookprep.devserver import make_server

    srv = make_server(tmp_path, "1234", port=0, fonts=True, screen="7.5in", update_site="/updates/")
    sd = tmp_path / "sd" / "books"
    (sd / ".src").mkdir(parents=True)
    assert prep([str(sample_epub), "--out", str(sd), "--screen", "7.5in-landscape"]) == 0
    shutil.copy(sample_epub, sd / ".src" / "testboek.epub")
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        port = srv.server_address[1]
        url = f"http://127.0.0.1:{port}/?k=1234#reader=testboek.t5b&find=alinea"
        dom = subprocess.run(
            [chromium(), "--headless=new", "--disable-gpu", "--no-first-run",
             "--virtual-time-budget=60000", "--dump-dom", url],
            capture_output=True, text=True, timeout=300, check=True,
        ).stdout  # fmt: skip
    finally:
        srv.shutdown()
    assert "made for landscape" in dom
    page = re.search(r'<canvas id="r-page"[^>]*style="([^"]*)"', dom)
    assert page and "800 / 480" in page.group(1).replace("/", " / ").replace("  ", " "), dom[-800:]
    hits = re.search(r'<body[^>]*data-hits="(\d+)"', dom)
    assert hits and int(hits.group(1)) >= 1, dom[-800:]


def test_page_installs_the_latest_release(tmp_path):
    """Update firmware -> Install: the page reads manifest-<board>.json from the update
    site (GitHub Pages; here a folder on the dev server), downloads the app image and
    sends it to /api/firmware under the release's file name."""
    from bookprep.devserver import make_server

    srv = make_server(tmp_path, "1234", port=0, fonts=False, update_site="/updates/")
    site = tmp_path / "www" / "updates"
    (site / "t5-213").mkdir(parents=True)
    app = bytes(range(256)) * 700  # 175 KB, a few download chunks
    (site / "t5-213" / "firmware.bin").write_bytes(app)
    parts = [{"path": "t5-213/bootloader.bin", "offset": 0x1000},
             {"path": "t5-213/firmware.bin", "offset": 0x10000}]  # fmt: skip
    manifest = {"version": "9.9.0", "builds": [{"chipFamily": "ESP32", "parts": parts}]}
    (site / "manifest-t5-213.json").write_text(json.dumps(manifest))
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        url = f"http://127.0.0.1:{srv.server_address[1]}/?k=1234#update"
        dom = subprocess.run(
            [chromium(), "--headless=new", "--disable-gpu", "--no-first-run",
             "--virtual-time-budget=60000", "--dump-dom", url],
            capture_output=True, text=True, timeout=300, check=True,
        ).stdout  # fmt: skip
    finally:
        srv.shutdown()
    status = re.search(r'<body[^>]*data-update="([^"]*)"', dom)
    assert status, dom[-800:]
    assert html.unescape(status.group(1)).startswith("sub ok: Updated."), status.group(1)
    assert (tmp_path / "firmware" / "t5-reader-9.9.0-ota.bin").read_bytes() == app
