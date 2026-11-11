"""The book server (bookprep.bookserver): profiles, the reader's text API, conversion
through bookprep, parts, the cache. Contract: firmware/src/bookserver.cpp."""

import os
import shutil
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

import pytest
from conftest import make_epub

from bookprep.bookserver import (
    PROTOCOL,
    Converter,
    Fonts,
    Profile,
    build_app,
    default_profile,
    fit,
    make_server,
    parse_profiles,
)
from bookprep.t5b import T5BFile, part_size


def get(url):
    try:
        with urllib.request.urlopen(url, timeout=30) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def records(body: bytes) -> list[list[str]]:
    lines = body.decode("utf-8").splitlines()
    assert lines[0] == PROTOCOL
    return [line.split("\t") for line in lines[1:]]


@pytest.fixture
def library(tmp_path, font_path):
    root = tmp_path / "books"
    root.mkdir()
    make_epub(root / "Testboek - A. Schrijver.epub")
    (root / "Later.txt").write_text("Een tweede boek.\n\nMet twee alinea's.\n", encoding="utf-8")
    fonts = tmp_path / "fonts"
    fonts.mkdir()
    shutil.copy(font_path, fonts / "Mijn Letter.ttf")  # a font of your own, by its name
    return root, fonts


@pytest.fixture
def server(tmp_path, library):
    root, fonts = library
    options = {
        "library": str(root),
        "profiles": [
            {"name": "Klein", "screen": "t5-213", "size": 12, "font": "mijn letter"},
            {"name": "Ruim", "screen": "t5-213", "line_spacing": 1.5, "justify": False},
            {"name": "Kapot", "screen": "4.7in", "font": "Geen Zulke Letter"},
        ],
    }
    app = build_app(options, tmp_path / "cache", tmp_path / "fontcache", [fonts])
    srv = make_server(app, "127.0.0.1", 0)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{srv.server_address[1]}", app
    srv.shutdown()
    app.converter.stop()


def wait_ready(base, query, timeout=120):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        status, body = get(f"{base}/api/prepare?{query}")
        assert status == 200
        rows = records(body)
        if rows[0][0] != "busy":
            return rows
        assert 0 <= int(rows[0][1]) <= 100 and int(rows[0][2]) >= 0
        time.sleep(0.3)
    raise AssertionError("conversion did not finish")


def test_profile_options_become_bookprep_options():
    p = Profile("x", "X", "7.5in", size=20, line_spacing=1.5, margin=0, indent=0,
                paragraph_spacing=4, justify=False, hyphenate=False, language="en",
                weight=150, bionic=True, cover=False, chapter_break=False)  # fmt: skip
    assert p.argv(Path("/f.ttf")) == [
        "--screen", "7.5in", "--font", "/f.ttf", "--threshold", "150", "--size", "20",
        "--line-height", "30", "--margin", "0", "--indent", "0", "--paragraph-spacing", "4",
        "--lang", "en", "--no-justify", "--no-hyphenate", "--bionic", "--no-cover",
        "--no-chapter-break",
    ]  # fmt: skip
    # defaults: only screen, font and weight; spacing applies to the screen's size
    assert Profile("y", "Y", "t5-213").argv(Path("f")) == [
        "--screen", "t5-213", "--font", "f", "--threshold", "128",
    ]  # fmt: skip
    assert "--line-height" in (a := Profile("z", "Z", "t5-213", line_spacing=1.5).argv(Path("f")))
    assert a[a.index("--line-height") + 1] == "20"  # ceil(13 * 1.5)


def test_bad_profiles_are_left_out_and_ids_stay_unique(capsys):
    profiles = parse_profiles([
        {"name": "Groot", "screen": "7.5in", "size": "22"},
        {"name": "Groot", "screen": "7.5in-landscape"},
        {"name": "", "screen": "7.5in"},
        {"name": "Geen scherm", "screen": "kindle"},
        {"name": "Raar", "screen": "t5-213", "size": "groot"},
    ])  # fmt: skip
    assert [(p.id, p.screen, p.size) for p in profiles] == [
        ("groot", "7.5in", 22), ("groot-2", "7.5in-landscape", None), ("raar", "t5-213", None),
    ]  # fmt: skip
    out = capsys.readouterr().out
    assert "profile 3: needs a name and a screen" in out and "profile 4" in out
    assert "size 'groot' is not a int; ignored" in out


def test_fields_have_no_tabs_or_breaks_and_fit_the_reader():
    assert fit("a\tb\nc  d") == "a b c d"
    assert fit("é" * 60) == "é" * 45  # 90 bytes, cut on a character
    assert fit("ab" + "é" * 50, 5) == "abé"


def test_profiles_per_screen_with_a_default_for_the_others(server):
    base, _ = server
    assert records(get(f"{base}/api/profiles?screen=t5-213")[1]) == [
        ["profile", "klein", "Klein"], ["profile", "ruim", "Ruim"],
    ]  # fmt: skip
    assert records(get(f"{base}/api/profiles?screen=7.5in")[1]) == [
        ["profile", "default-7.5in", "Default"]
    ]
    status, body = get(f"{base}/api/profiles?screen=kindle")
    assert status == 400 and records(body) == [["error", "unknown screen 'kindle'"]]


def test_lists_newest_titles_authors_and_paging(server):
    base, _ = server
    books = records(get(f"{base}/api/books?sort=title")[1])
    assert books[0] == ["total", "2"]
    assert [r[2:] for r in books[1:]] == [
        ["Later", "", "Later.t5b"],
        ["Testboek", "A. Schrijver", "Testboek_-_A._Schrijver.t5b"],
    ]
    page = records(get(f"{base}/api/books?sort=title&offset=1&count=1")[1])
    assert page == [["total", "2"], books[2]]
    newest = records(get(f"{base}/api/books?sort=new")[1])
    assert sorted(newest[1:]) == sorted(books[1:])
    authors = records(get(f"{base}/api/authors")[1])
    assert authors[0] == ["total", "1"] and authors[1][2:] == ["A. Schrijver", "1"]
    by_author = records(get(f"{base}/api/books?author={authors[1][1]}")[1])
    assert by_author == [["total", "1"], books[2]]
    assert get(f"{base}/api/books?author=nobody")[0] == 404


def test_prepare_download_and_parts(server):
    base, app = server
    book = app.library.by_title()[1]  # the EPUB
    query = f"profile=klein&book={book.id}"
    (ready,) = wait_ready(base, query)
    assert ready[0] == "ready" and ready[3] == "Testboek_-_A._Schrijver.t5b"
    pages, size = int(ready[1]), int(ready[2])
    status, data = get(f"{base}/api/download?{query}")
    assert status == 200 and len(data) == size
    cached = app.converter.status(book, app.profile("klein")).path
    assert data == cached.read_bytes()
    t5b = T5BFile(cached)
    assert t5b.header.page_count == pages and t5b.header.page_w == 250
    # with room for the book: no parts; with less: parts that each fit
    assert len(wait_ready(base, f"{query}&room={size}")) == 1
    room = part_size(t5b, 0, 1) + 10
    rows = wait_ready(base, f"{query}&room={room}")
    parts = [(int(r[1]), int(r[2]), int(r[3])) for r in rows[1:]]
    assert rows[1][0] == "part" and parts[0][0] == 1 and parts[-1][1] == pages
    assert all(b <= room for _, _, b in parts)
    first, last, nbytes = parts[-1]
    status, data = get(f"{base}/api/download?{query}&first={first}&last={last}")
    assert status == 200 and len(data) == nbytes
    assert get(f"{base}/api/download?{query}&first=2&last={pages + 1}")[0] == 400
    assert records(get(f"{base}/api/prepare?{query}&room=50")[1]) == [ready]  # nothing fits


def test_unknown_book_profile_font_and_not_ready(server):
    base, app = server
    book = app.library.by_title()[0]
    status, body = get(f"{base}/api/prepare?profile=nope&book={book.id}")
    assert status == 404 and records(body)[0][0] == "error"
    assert get(f"{base}/api/prepare?profile=klein&book=nope")[0] == 404
    (err,) = records(get(f"{base}/api/prepare?profile=kapot&book={book.id}")[1])
    assert err[0] == "error" and "font 'Geen Zulke Letter' not found" in err[1]
    status, body = get(f"{base}/api/download?profile=kapot&book={book.id}")
    assert status == 409 and "not found" in records(body)[0][1]


def test_a_book_that_cannot_be_converted_reports_why(server):
    base, app = server
    bad = app.library.root / "kapot.epub"
    bad.write_bytes(b"not a zip")
    app.library._scanned = 0  # rescan now
    book = next(b for b in app.library.scan() if b.rel == "kapot.epub")
    (err,) = wait_ready(base, f"profile=ruim&book={book.id}")
    assert err[0] == "error" and err[1]
    assert get(f"{base}/api/download?profile=ruim&book={book.id}")[0] == 409


def test_page_for_people_and_health(server):
    base, app = server
    status, body = get(f"{base}/")
    assert status == 200 and b"Testboek" in body and b"Klein" in body
    assert b"FireBeetle 7.5&quot; (portrait)" in body  # the default profile of that screen
    assert get(f"{base}/health") == (200, b"ok\n")
    assert get(f"{base}/nothing")[0] == 404
    book = app.library.by_title()[0]
    status, body = get(f"{base}/get?profile=ruim&book={book.id}")
    assert status == 200  # "Typesetting ...", or the file when it was quick


def test_cache_keys_follow_options_and_font_and_old_profiles_go(tmp_path, font_path):
    fonts = Fonts([], tmp_path / "fonts")
    conv = Converter(tmp_path / "cache", fonts, max_bytes=1000)
    try:
        a, _ = conv.profile_key(Profile("a", "A", "t5-213"))
        assert conv.profile_key(Profile("b", "B", "t5-213"))[0] == a  # the name doesn't count
        assert conv.profile_key(Profile("a", "A", "t5-213", size=14))[0] != a
        assert conv.profile_key(default_profile("7.5in"))[0] != a
        for key, name, size, age in ((a, "old.t5b", 600, 100), (a, "new.t5b", 600, 50),
                                     ("gone", "x.t5b", 10, 10)):  # fmt: skip
            path = tmp_path / "cache" / key / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"x" * size)
            stamp = time.time() - age
            os.utime(path, (stamp, stamp))
        conv.prune({a})
        assert sorted(p.name for p in (tmp_path / "cache").rglob("*.t5b")) == ["new.t5b"]
    finally:
        conv.stop()


def test_bundled_fonts_by_name_and_own_fonts_by_file(tmp_path, font_path):
    own = tmp_path / "own"
    own.mkdir()
    shutil.copy(font_path, own / "Huisletter-Regular.otf")
    fonts = Fonts([own], tmp_path / "cache")
    assert fonts.resolve("").name == "Ubuntu-R.ttf"  # the env's, as the CLI's default
    assert fonts.resolve("Ubuntu") == fonts.resolve("ubuntu")
    assert fonts.resolve("huisletter regular").name == "Huisletter-Regular.otf"
    assert fonts.resolve(str(font_path)) == font_path
    with pytest.raises(LookupError, match="bundled: Ubuntu, Literata"):
        fonts.resolve("Comic Sans")
