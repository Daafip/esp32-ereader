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
    assert book == {"file": "boek.t5b", "folder": "", "title": "Testboek", "author": "A",
                    "pages": 2, "size": book["size"], "position": 0, "progress": 0,
                    "cover": False, "marks": 0, "source": "boek.epub"}  # fmt: skip
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


def test_full_card_is_refused_with_the_free_space(tmp_path):
    srv = make_server(tmp_path, CODE, port=0, fonts=False, card_mb=0.05)  # 52 KB card
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{srv.server_address[1]}"
    try:
        data = info(base)
        assert data["total"] == int(0.05 * 1048576) and data["free"] == data["total"]
        status, body = upload(base, "book", "boek.t5b", t5b_bytes(tmp_path))
        assert status == 200
        free = json.loads(body)["free"]
        assert free == info(base)["free"] < data["total"]
        status, body = upload(base, "source", "boek.epub", bytes(free + 1))
        assert status == 507
        assert json.loads(body) == {"error": "not enough space on the SD card", "free": free}
        assert not (tmp_path / "sd" / "books" / ".src" / "boek.epub").exists()
        status, body = upload(base, "book", "x" * 61 + ".t5b", t5b_bytes(tmp_path))
        assert status == 400 and b"too long" in body
    finally:
        srv.shutdown()


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


def post(base, path, **fields):
    from urllib.parse import urlencode

    body = urlencode(fields).encode()
    return call(f"{base}{path}?k={CODE}", "POST", body,
                {"Content-Type": "application/x-www-form-urlencoded"})  # fmt: skip


def test_folders_marks_position_and_pages(server, tmp_path):
    base, root = server
    assert upload(base, "book", "boek.t5b", t5b_bytes(tmp_path, title="Een"))[0] == 200
    assert post(base, "/api/move", file="boek.t5b", folder="Klassiek")[0] == 200
    (book,) = info(base)["books"]
    assert (book["file"], book["folder"]) == ("Klassiek/boek.t5b", "Klassiek")
    # a second upload of the same name replaces it in its folder
    assert upload(base, "book", "boek.t5b", t5b_bytes(tmp_path, title="Een"))[0] == 200
    assert [b["file"] for b in info(base)["books"]] == ["Klassiek/boek.t5b"]
    assert post(base, "/api/move", file="../boek.t5b", folder="x")[0] == 400

    assert json.loads(post(base, "/api/marks", file="Klassiek/boek.t5b", page="1")[1]) == {
        "pages": [1]
    }
    assert post(base, "/api/marks", file="Klassiek/boek.t5b", page="9")[0] == 400
    _, body = call(f"{base}/api/marks?file=Klassiek/boek.t5b&k={CODE}")
    assert json.loads(body)["pages"] == [1]
    assert info(base)["books"][0]["marks"] == 1

    assert post(base, "/api/position", file="Klassiek/boek.t5b", page="1")[0] == 200
    assert info(base)["books"][0]["position"] == 1
    status, page = call(f"{base}/api/page?file=Klassiek/boek.t5b&n=0&k={CODE}")
    assert (status, len(page)) == (200, 32 * 122)

    # moving back to the root removes the empty folder
    assert post(base, "/api/move", file="Klassiek/boek.t5b", folder="")[0] == 200
    assert not (root / "sd" / "books" / "Klassiek").exists()
    assert info(base)["books"][0]["position"] == 1  # the position moved along


def test_settings_name_wifi_time_pictures_reset(server, tmp_path):
    base, root = server
    _, body = call(f"{base}/api/settings?k={CODE}")
    settings = {s["key"]: s for s in json.loads(body)["settings"]}
    assert settings["language"]["options"] == ["English", "Nederlands"]
    assert post(base, "/api/settings", key="language", value="1")[0] == 200
    assert info(base)["lang"] == "nl"
    assert post(base, "/api/settings", key="language", value="7")[0] == 400

    assert post(base, "/api/name", name="Davids reader")[0] == 200
    assert info(base)["name"] == "Davids reader"
    assert post(base, "/api/wifi", ssid="Thuis", password="geheim123")[0] == 200
    assert info(base)["homeWifi"] == "Thuis"
    assert "geheim123" not in (root / "state.json").read_text()  # never stored or sent back
    assert call(f"{base}/api/wifi?k={CODE}", "DELETE")[0] == 200
    assert info(base)["homeWifi"] == ""

    assert post(base, "/api/time", epoch="1760000000", tz="7200")[0] == 200
    assert info(base)["stats"]["clock"] is True
    assert post(base, "/api/time", epoch="12", tz="0")[0] == 400

    assert upload(base, "sleep", "a.bin", bytes(32 * 122))[0] == 200
    assert upload(base, "sleep", "b.bin", bytes(100))[0] == 400
    assert info(base)["pictures"] == ["a.bin"]
    assert call(f"{base}/api/picture?name=a.bin&k={CODE}", "DELETE")[0] == 200

    assert upload(base, "book", "boek.t5b", t5b_bytes(tmp_path))[0] == 200
    assert post(base, "/api/reset")[0] == 400
    assert post(base, "/api/reset", confirm="erase")[0] == 200
    data = info(base)
    assert (data["books"], data["name"], data["lang"]) == ([], "", "en")
