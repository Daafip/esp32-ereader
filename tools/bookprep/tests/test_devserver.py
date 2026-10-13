"""The development server implements the reader's web API (same contract as
firmware/src/transfer.cpp); these tests pin that contract down."""

import json
import threading
import urllib.error
import urllib.request
from pathlib import Path

import pytest
from PIL import Image

from bookprep.devserver import is_safe_name, make_server
from bookprep.t5b import TocEntry, pack_page, write_t5b

CODE = "4821"


@pytest.fixture
def server(tmp_path):
    srv = make_server(tmp_path, CODE, port=0, fonts=False)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{srv.server_address[1]}", tmp_path
    srv.shutdown()


def call(url, method="GET", data=None, headers=None):
    req = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def upload(base, kind, name, data, code=CODE):
    boundary = "----t5test"
    body = (
        (
            f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="{name}"\r\n'
            "Content-Type: application/octet-stream\r\n\r\n"
        ).encode()
        + data
        + f"\r\n--{boundary}--\r\n".encode()
    )
    return call(f"{base}/api/upload?kind={kind}&k={code}", "POST", body,
                {"Content-Type": f"multipart/form-data; boundary={boundary}"})  # fmt: skip


def t5b_bytes(tmp_path: Path, title="Testboek", w=250, h=122) -> bytes:
    path = tmp_path / "make.t5b"
    page = pack_page(Image.new("1", (w, h), 1))
    write_t5b(path, title=title, author="A", page_w=w, page_h=h, toc=[TocEntry(0, "Een")],
              pages=[page, page])  # fmt: skip
    return path.read_bytes()


def info(base):
    status, body = call(f"{base}/api/info?k={CODE}")
    assert status == 200
    return json.loads(body)


def test_page_and_converter_assets_are_served(server):
    base, _ = server
    for path in ("/", "/bookprep.js", "/hyph/index.json", "/hyph/nl_NL.txt"):
        status, body = call(base + path)
        assert status == 200, path
        assert body
    assert "nl" in json.loads(call(base + "/hyph/index.json")[1])["aliases"]


def test_code_required(server):
    base, _ = server
    assert call(f"{base}/api/info")[0] == 403
    assert call(f"{base}/api/info?k=0000")[0] == 403
    assert upload(base, "book", "x.t5b", b"", code="0000")[0] == 403


def test_upload_book_source_meta_font_and_restyle_flow(server, tmp_path):
    base, root = server
    status, body = upload(base, "book", "boek.t5b", t5b_bytes(tmp_path))
    assert status == 200
    assert json.loads(body)["title"] == "Testboek"
    assert upload(base, "source", "boek.epub", b"PK fake epub")[0] == 200
    assert upload(base, "meta", "boek.json", json.dumps({"size": 14}).encode())[0] == 200
    assert upload(base, "font", "Mijn Font.ttf", b"\0\1\0\0 fake")[0] == 200

    data = info(base)
    (book,) = data["books"]
    assert book == {"file": "boek.t5b", "title": "Testboek", "author": "A", "pages": 2,
                    "size": book["size"], "progress": 0, "source": "boek.epub"}  # fmt: skip
    assert data["fonts"] == [{"name": "Mijn Font.ttf", "size": 9}]

    assert call(f"{base}/file?kind=source&name=boek.epub&k={CODE}")[1] == b"PK fake epub"
    assert json.loads(call(f"{base}/file?kind=meta&name=boek.json&k={CODE}")[1]) == {"size": 14}
    assert call(f"{base}/file?kind=source&name=boek.epub")[0] == 403

    assert call(f"{base}/api/book?file=boek.t5b&k={CODE}", "DELETE")[0] == 200
    assert info(base)["books"] == []
    assert not list((root / "sd" / "books" / ".src").iterdir())  # original and settings gone
    assert call(f"{base}/api/font?name=Mijn%20Font.ttf&k={CODE}", "DELETE")[0] == 200
    assert info(base)["fonts"] == []


def test_uploads_are_validated(server, tmp_path):
    base, root = server
    assert upload(base, "book", "boek.t5b", b"not a book")[0] == 400
    assert upload(base, "book", "klein.t5b", t5b_bytes(tmp_path, w=212, h=104))[0] == 400
    assert upload(base, "book", "boek.epub", t5b_bytes(tmp_path))[0] == 400
    assert upload(base, "source", "boek.exe", b"x")[0] == 400
    assert upload(base, "book", "../escape.t5b", t5b_bytes(tmp_path))[0] == 200  # path stripped
    assert (root / "sd" / "books" / "escape.t5b").exists()
    assert not (root / "sd" / "escape.t5b").exists()
    assert upload(base, "nope", "x.t5b", b"")[0] == 400
    assert not list((root / "sd" / "books").glob("*.part"))


def test_screen_is_reported_and_enforced(tmp_path):
    srv = make_server(tmp_path, CODE, port=0, fonts=False, screen="4.7in")
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{srv.server_address[1]}"
    try:
        assert info(base)["screen"] == {
            "id": "4.7in", "width": 540, "height": 960, "statusBar": 28, "size": 28, "margin": 24,
        }  # fmt: skip
        assert upload(base, "book", "klein.t5b", t5b_bytes(tmp_path))[0] == 400
        assert upload(base, "book", "groot.t5b", t5b_bytes(tmp_path, w=540, h=960))[0] == 200
    finally:
        srv.shutdown()


def test_default_screen_is_the_t5_213(server):
    base, _ = server
    assert info(base)["screen"]["id"] == "t5-213"
    assert (info(base)["screen"]["width"], info(base)["screen"]["height"]) == (250, 122)


@pytest.mark.parametrize(
    ("name", "ok"),
    [("boek.t5b", True), ("Mijn boek (2).T5B", True), (".t5b", False), (".hidden.t5b", False),
     ("a/b.t5b", False), ("a\\b.t5b", False), ("b.epub", False), ("x" * 61 + ".t5b", False)],
)  # fmt: skip
def test_safe_names_match_firmware_rules(name, ok):
    assert is_safe_name(name, (".t5b",)) is ok
