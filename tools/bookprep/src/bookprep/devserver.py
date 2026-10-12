"""Development stand-in for the reader's Wi-Fi transfer server.

Serves the same page and assets as the firmware and implements the same JSON
API (firmware/src/transfer.cpp), with a local folder as the SD card. Use it to
work on the page, or to try the in-browser converter from a phone without the
reader:

    pixi run web-dev            # http://localhost:8000
    pixi run web-dev --lan      # reachable from your phone on the same network
"""

from __future__ import annotations

import argparse
import contextlib
import email
import json
import random
import re
import shutil
import socket
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

from .t5b import T5BFile
from .webassets import build_assets, write_files

KINDS = {
    "book": ("books", (".t5b",), 64 << 20),
    "source": ("books/.src", (".epub", ".txt"), 64 << 20),
    "meta": ("books/.src", (".json",), 64 << 10),
    "font": ("fonts", (".ttf", ".otf", ".woff", ".woff2"), 8 << 20),
}
MIME = {".epub": "application/epub+zip", ".txt": "text/plain", ".json": "application/json",
        ".ttf": "font/ttf", ".otf": "font/otf", ".woff": "font/woff", ".woff2": "font/woff2",
        ".t5b": "application/octet-stream"}  # fmt: skip


def is_safe_name(name: str, exts: tuple[str, ...]) -> bool:
    """Same rules as isSafeFileName() in firmware/lib/core/src/web_util.cpp."""
    if not 1 <= len(name.encode()) <= 64 or name.startswith("."):
        return False
    if not name.lower().endswith(exts) or name.lower() in exts:
        return False
    return not re.search(r'[\x00-\x1f\x7f/\\:"*?<>|]', name)


class Handler(SimpleHTTPRequestHandler):
    root: Path
    code: str

    def log_message(self, fmt, *args):
        if not self.path.startswith(("/fonts/", "/hyph/")):
            super().log_message(fmt, *args)

    # --- helpers -----------------------------------------------------------------------

    def query(self) -> dict[str, str]:
        return {k: v[0] for k, v in parse_qs(urlparse(self.path).query).items()}

    def send_json(self, status: int, body: dict) -> None:
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def authorized(self) -> bool:
        if self.query().get("k") == self.code:
            return True
        self.send_json(403, {"error": "wrong code"})
        return False

    def sd(self, kind: str) -> Path:
        d = self.root / "sd" / KINDS[kind][0]
        d.mkdir(parents=True, exist_ok=True)
        return d

    # --- routes ------------------------------------------------------------------------

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/api/info":
            return self.info()
        if path == "/file":
            return self.get_file()
        self.path = path  # static: drop the query string
        return super().do_GET()

    def do_POST(self):
        path = urlparse(self.path).path
        if path == "/api/upload":
            return self.upload()
        if path == "/api/firmware":
            if self.authorized():
                self.read_upload()
                self.send_json(200, {})
            return None
        return self.send_json(404, {"error": "not found"})

    def do_DELETE(self):
        if not self.authorized():
            return None
        path = urlparse(self.path).path
        q = self.query()
        if path == "/api/book":
            name = q.get("file", "")
            if not is_safe_name(name, (".t5b",)):
                return self.send_json(400, {"error": "bad file name"})
            book = self.sd("book") / name
            if not book.exists():
                return self.send_json(404, {"error": "no such book"})
            book.unlink()
            for extra in self.sd("source").glob(Path(name).stem + ".*"):
                extra.unlink()
            return self.send_json(200, {})
        if path == "/api/font":
            name = q.get("name", "")
            if not is_safe_name(name, KINDS["font"][1]) or not (self.sd("font") / name).exists():
                return self.send_json(404, {"error": "no such font"})
            (self.sd("font") / name).unlink()
            return self.send_json(200, {})
        return self.send_json(404, {"error": "not found"})

    def info(self):
        if not self.authorized():
            return
        books = []
        for path in sorted(self.sd("book").glob("*.t5b")):
            try:
                book = T5BFile(path)
            except ValueError:
                continue
            originals = self.sd("source").glob(path.stem + ".*")
            source = next((p.name for p in originals if p.suffix in (".epub", ".txt")), None)
            books.append(
                {
                    "file": path.name,
                    "title": book.header.title or path.name,
                    "author": book.header.author,
                    "pages": book.header.page_count,
                    "size": path.stat().st_size,
                    "progress": 0,
                    "source": source,
                }
            )
        books.sort(key=lambda b: b["title"].lower())
        usage = shutil.disk_usage(self.root)
        font_files = sorted(self.sd("font").iterdir())
        fonts = [
            {"name": p.name, "size": p.stat().st_size}
            for p in font_files
            if p.suffix in KINDS["font"][1]
        ]
        self.send_json(200, {"version": "dev", "battery": -1, "total": usage.total,
                             "free": usage.free, "books": books, "fonts": fonts})  # fmt: skip

    def read_upload(self) -> tuple[str, bytes] | None:
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        msg = email.message_from_bytes(
            f"Content-Type: {self.headers.get('Content-Type', '')}\r\n\r\n".encode() + body
        )
        for part in msg.get_payload() if msg.is_multipart() else []:
            if part.get_filename():
                return part.get_filename(), part.get_payload(decode=True)
        return None

    def upload(self):
        if not self.authorized():
            return None
        kind = self.query().get("kind", "book")
        if kind not in KINDS:
            return self.send_json(400, {"error": "unknown kind"})
        got = self.read_upload()
        if not got:
            return self.send_json(400, {"error": "no file"})
        name, data = got
        name = name.replace("\\", "/").rsplit("/", 1)[-1]
        _, exts, limit = KINDS[kind]
        if not is_safe_name(name, exts):
            return self.send_json(400, {"error": f"only {'/'.join(exts)} files with a plain name"})
        if len(data) > limit:
            return self.send_json(413, {"error": "file too large"})
        dest = self.sd(kind) / name
        tmp = dest.with_name(dest.name + ".part")
        tmp.write_bytes(data)
        title = name
        if kind == "book":
            try:
                book = T5BFile(tmp)
                if (book.header.page_w, book.header.page_h) != (250, 122):
                    raise ValueError("made for another screen size")
                title = book.header.title or name
            except ValueError as e:
                tmp.unlink()
                return self.send_json(400, {"error": str(e)})
        tmp.replace(dest)
        return self.send_json(200, {"ok": 1, "title": title})

    def get_file(self):
        if not self.authorized():
            return None
        q = self.query()
        kind, name = q.get("kind", ""), q.get("name", "")
        if kind not in ("source", "meta", "font") or not is_safe_name(name, KINDS[kind][1]):
            return self.send_json(400, {"error": "bad request"})
        path = self.sd(kind) / name
        if not path.exists():
            return self.send_json(404, {"error": "not found"})
        data = path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", MIME.get(path.suffix.lower(), "application/octet-stream"))
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        return None


def lan_address() -> str:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        try:
            s.connect(("192.0.2.1", 9))  # no packet is sent
            return s.getsockname()[0]
        except OSError:
            return "127.0.0.1"


def make_server(
    root: Path, code: str, host: str = "127.0.0.1", port: int = 8000, fonts: bool = True
) -> ThreadingHTTPServer:
    root.mkdir(parents=True, exist_ok=True)
    write_files(build_assets(Path("firmware/.pio/fontcache"), fonts=fonts), root / "www")

    class Bound(Handler):
        pass

    Bound.root = root
    Bound.code = code

    def factory(*args, **kwargs):
        return Bound(*args, directory=str(root / "www"), **kwargs)

    return ThreadingHTTPServer((host, port), factory)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="web-dev", description=__doc__.splitlines()[0])
    p.add_argument("--root", type=Path, default=Path("out/web-dev"), help="assets + fake SD card")
    p.add_argument("--port", type=int, default=8000)
    p.add_argument("--lan", action="store_true", help="listen on all interfaces (phone access)")
    p.add_argument("--code", default=None, help="session code (default: random)")
    args = p.parse_args(argv)
    code = args.code or f"{random.randrange(10000):04d}"
    server = make_server(args.root, code, "0.0.0.0" if args.lan else "127.0.0.1", args.port)
    host = lan_address() if args.lan else "localhost"
    print(f"T5 reader dev server: http://{host}:{args.port}/?k={code}   (code {code})")
    print(f"fake SD card: {args.root / 'sd'}   Ctrl+C to stop")
    with contextlib.suppress(KeyboardInterrupt):
        server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
