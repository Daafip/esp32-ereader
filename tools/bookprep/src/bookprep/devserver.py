"""Development stand-in for the reader's Wi-Fi transfer server.

Serves the same page and assets as the firmware and implements the same JSON
API (firmware/src/transfer.cpp), with a local folder as the SD card. Use it to
work on the page, or to try the in-browser converter from a phone without the
reader:

    pixi run web-dev                    # http://localhost:8000
    pixi run web-dev --lan              # reachable from your phone on the same network
    pixi run web-dev --screen 4.7in     # pretend to be a 4.7" reader (T5-4.7", M5Paper)
    pixi run web-dev --screen 7.5in     # the 7.5" (7.5in-landscape: turned to landscape)

The card layout matches the reader's (firmware/src/storage.h): /books (one folder
level), /books/.src, /books/.marks, /sleep, /fonts. Reader settings, the device
name, home Wi-Fi, positions and the clock live in <root>/state.json. A firmware
update is kept in <root>/firmware/<name>.

Unlike the reader, /api/info can name the page's update site (--update-site, e.g.
a local folder under <root>/www for the tests); without it the page asks GitHub.
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
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

from .layout import DEFAULT_SCREEN, SCREENS, Screen
from .t5b import T5BFile, stride_for
from .webassets import build_assets, write_files

KINDS = {
    "book": ("books", (".t5b",), 64 << 20),
    "source": ("books/.src", (".epub", ".txt"), 64 << 20),
    "meta": ("books/.src", (".json",), 64 << 10),
    "font": ("fonts", (".ttf", ".otf", ".woff", ".woff2"), 8 << 20),
    "sleep": ("sleep", (".bin",), 0),  # one page, or a square (picture_sizes)
}
MIME = {".epub": "application/epub+zip", ".txt": "text/plain", ".json": "application/json",
        ".ttf": "font/ttf", ".otf": "font/otf", ".woff": "font/woff", ".woff2": "font/woff2",
        ".t5b": "application/octet-stream", ".bin": "application/octet-stream"}  # fmt: skip
# /api/info "board" (firmware: BOARD_NAME), one per screen size for the dev server.
# The 7.5" board has two screens (Settings -> Orientation; firmware: kScreens).
BOARD_FOR_SCREEN = {"t5-213": "t5-213", "4.7in": "t5-47", "7.5in": "firebeetle-75",
                    "7.5in-landscape": "firebeetle-75"}  # fmt: skip

# The firmware's settings (lib/core/src/settings.cpp): key, English label, options,
# default option. The page shows whatever the reader sends.
SETTINGS = [
    ("sleepScreen", "Sleep screen",
     ["book", "cover", "pictures", "keep page", "picture + book"], 0),
    ("statusBar", "Status bar", ["full", "minimal", "hidden"], 0),
    ("flip", "Flip screen", ["off", "on"], 0),
    ("holdAction", "Long hold", ["sleep", "bookmark", "lock", "menu"], 0),
    ("lockOnSleep", "Lock when asleep", ["off", "on"], 0),
    ("language", "Language", ["English", "Nederlands"], 0),
    ("fullEvery", "Full refresh",
     ["every 5", "every 10", "every 15", "every 20", "every 30", "chapters only"], 3),
    ("longPress", "Hold time", ["400 ms", "500 ms", "600 ms", "800 ms", "1000 ms"], 2),
    ("idleMinutes", "Idle sleep", ["2 min", "5 min", "10 min", "20 min", "30 min", "60 min"], 2),
    ("deepSleep", "Sleep after page", ["off", "on"], 1),
    ("wakeRefresh", "Wake refresh", ["rewrite", "retain", "full"], 0),
]  # fmt: skip
# "flip" on boards that turn (setOrientationCount(4) in settings.cpp)
ORIENTATION = ("flip", "Orientation", ["portrait", "portrait 180\N{DEGREE SIGN}", "landscape",
               "landscape 180\N{DEGREE SIGN}"], 0)  # fmt: skip


def board_screens(screen: Screen) -> list[Screen]:
    """The screens of the board this one belongs to, its first screen first (kScreens)."""
    board = BOARD_FOR_SCREEN.get(screen.id, screen.id)
    return [s for s in SCREENS.values() if BOARD_FOR_SCREEN.get(s.id, s.id) == board]


def settings_for(screen: Screen) -> list[tuple]:
    """SETTINGS as the reader with this screen has them: a board that turns has four
    orientations under "flip", the turned screen (index 2) by default here."""
    screens = board_screens(screen)
    if len(screens) < 2:
        return SETTINGS
    turn = (*ORIENTATION[:3], 2 if screen.id != screens[0].id else 0)
    return [turn if s[0] == "flip" else s for s in SETTINGS]


def picture_sizes(screen: Screen) -> tuple[int, int]:
    """Bytes of a full-screen and of a square sleep picture (kPageBytes, kSquareBytes)."""
    side = min(screen.width, screen.height)
    return stride_for(screen.width) * screen.height, stride_for(side) * side


def is_safe_name(name: str, exts: tuple[str, ...]) -> bool:
    """Same rules as isSafeFileName() in firmware/lib/core/src/web_util.cpp."""
    if not 1 <= len(name.encode()) <= 64 or name.startswith("."):
        return False
    if not name.lower().endswith(exts) or name.lower() in exts:
        return False
    return not re.search(r'[\x00-\x1f\x7f/\\:"*?<>|]', name)


def is_safe_folder(name: str) -> bool:
    """isSafeFolderName() in web_util.cpp."""
    if not 1 <= len(name.encode()) <= 32 or name.startswith(".") or name.endswith(" "):
        return False
    return not re.search(r'[\x00-\x1f\x7f/\\:"*?<>|]', name)


def is_safe_book_path(path: str) -> bool:
    """isSafeBookPath() in web_util.cpp: "x.t5b" or "Folder/x.t5b"."""
    parts = path.split("/")
    if len(parts) == 1:
        return is_safe_name(parts[0], (".t5b",))
    return len(parts) == 2 and is_safe_folder(parts[0]) and is_safe_name(parts[1], (".t5b",))


def remap_page(page: int, old: int, new: int) -> int:
    """remapPage() in bookmarks.cpp."""
    if new == 0 or old <= 1 or new == 1:
        return 0
    return min(new - 1, (page * (new - 1) + (old - 1) // 2) // (old - 1))


class Handler(SimpleHTTPRequestHandler):
    root: Path
    code: str
    screen: Screen
    card_bytes: int | None = None  # a fake card this small (--card-mb), else the real disk
    update_site: str | None = None  # the page's releases (--update-site), else GitHub

    def log_message(self, fmt, *args):
        if not self.path.startswith(("/fonts/", "/hyph/")):
            super().log_message(fmt, *args)

    # --- helpers -----------------------------------------------------------------------

    def query(self) -> dict[str, str]:
        q = {k: v[0] for k, v in parse_qs(urlparse(self.path).query).items()}
        q.update(getattr(self, "_form", {}))
        return q

    def read_form(self) -> None:
        """POST bodies in application/x-www-form-urlencoded count as arguments too
        (like the firmware's WebServer)."""
        self._form = {}
        if "application/x-www-form-urlencoded" in self.headers.get("Content-Type", ""):
            body = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()
            self._form = {k: v[0] for k, v in parse_qs(body, keep_blank_values=True).items()}

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

    def card(self) -> Path:
        return self.root / "sd"

    def card_space(self) -> tuple[int, int]:
        """(total, free) bytes of the fake card."""
        usage = shutil.disk_usage(self.root)
        if not self.card_bytes:
            return usage.total, usage.free
        files = (p for p in self.card().rglob("*") if p.is_file())
        used = sum(p.stat().st_size for p in files)
        return self.card_bytes, max(0, min(usage.free, self.card_bytes - used))

    def sd(self, kind: str) -> Path:
        d = self.card() / KINDS[kind][0]
        if kind == "sleep" and self.screen.id != board_screens(self.screen)[0].id:
            d = d / self.screen.id  # storage::picturesDir(): a turned screen's own folder
        d.mkdir(parents=True, exist_ok=True)
        return d

    def marks_file(self, stem: str) -> Path:
        return self.card() / "books" / ".marks" / f"{stem}.txt"

    def state(self) -> dict:
        path = self.root / "state.json"
        state = json.loads(path.read_text()) if path.exists() else {}
        state.setdefault("settings", {k: d for k, _, _, d in settings_for(self.screen)})
        state.setdefault("positions", {})
        state.setdefault("name", "")
        state.setdefault("wifi", {})
        return state

    def save_state(self, state: dict) -> None:
        (self.root / "state.json").write_text(json.dumps(state, indent=1))

    def book_screen(self, book: T5BFile) -> Screen | None:
        """The board screen a book was made for (storage::checkHeader), if any."""
        size = (book.header.page_w, book.header.page_h)
        return next((s for s in board_screens(self.screen) if (s.width, s.height) == size), None)

    def books(self) -> list[tuple[str, Path, T5BFile]]:
        """(relative path, file, book) for every valid book, folders one level deep."""
        root = self.sd("book")
        out = []
        for path in sorted([*root.glob("*.t5b"), *root.glob("*/*.t5b")]):
            if path.parent != root and path.parent.name.startswith("."):
                continue
            with contextlib.suppress(ValueError):
                book = T5BFile(path)
                if self.book_screen(book):
                    out.append((path.relative_to(root).as_posix(), path, book))
        return out

    def book_arg(self) -> tuple[str, Path, T5BFile] | None:
        rel = self.query().get("file", "")
        if not is_safe_book_path(rel):
            self.send_json(400, {"error": "bad file name"})
            return None
        path = self.sd("book") / rel
        try:
            return rel, path, T5BFile(path)
        except (OSError, ValueError):
            self.send_json(404, {"error": "no such book"})
            return None

    def read_marks(self, stem: str, pages: int) -> list[int]:
        f = self.marks_file(stem)
        if not f.exists():
            return []
        lines = f.read_text().splitlines()
        return sorted({int(x) for x in lines if x.strip().isdigit() and int(x) < pages})

    def write_marks(self, stem: str, marks: list[int]) -> None:
        f = self.marks_file(stem)
        if not marks:
            f.unlink(missing_ok=True)
            return
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_text("".join(f"{m}\n" for m in sorted(set(marks))[:100]))

    # --- routes ------------------------------------------------------------------------

    def do_GET(self):
        path = urlparse(self.path).path
        routes = {"/api/info": self.info, "/file": self.get_file, "/api/marks": self.marks,
                  "/api/page": self.page, "/api/settings": self.settings}  # fmt: skip
        if path in routes:
            return routes[path]()
        self.path = path  # static: drop the query string
        return super().do_GET()

    def do_POST(self):
        path = urlparse(self.path).path
        if path == "/api/upload":
            return self.upload()
        if path == "/api/firmware":
            if self.authorized():
                got = self.read_upload()
                if not got:
                    return self.send_json(400, {"error": "no file"})
                name, data = got
                name = name.rsplit("/", 1)[-1]
                if not is_safe_name(name, (".bin",)):
                    name = "firmware.bin"
                (self.root / "firmware").mkdir(exist_ok=True)
                (self.root / "firmware" / name).write_bytes(data)
                self.send_json(200, {})
            return None
        self.read_form()
        routes = {"/api/move": self.move, "/api/marks": self.marks, "/api/position": self.position,
                  "/api/settings": self.settings, "/api/name": self.name, "/api/wifi": self.wifi,
                  "/api/time": self.set_time, "/api/reset": self.reset}  # fmt: skip
        if path in routes:
            return routes[path]()
        return self.send_json(404, {"error": "not found"})

    def do_DELETE(self):
        if not self.authorized():
            return None
        path = urlparse(self.path).path
        q = self.query()
        if path == "/api/book":
            got = self.book_arg()
            if not got:
                return None
            _, book_path, _ = got
            book_path.unlink()
            for extra in self.sd("source").glob(book_path.stem + ".*"):
                extra.unlink()
            self.marks_file(book_path.stem).unlink(missing_ok=True)
            if book_path.parent != self.sd("book"):
                with contextlib.suppress(OSError):
                    book_path.parent.rmdir()  # only when empty
            return self.send_json(200, {})
        if path == "/api/font":
            name = q.get("name", "")
            if not is_safe_name(name, KINDS["font"][1]) or not (self.sd("font") / name).exists():
                return self.send_json(404, {"error": "no such font"})
            (self.sd("font") / name).unlink()
            return self.send_json(200, {})
        if path == "/api/picture":
            name = q.get("name", "")
            if not is_safe_name(name, (".bin",)) or not (self.sd("sleep") / name).exists():
                return self.send_json(404, {"error": "no such picture"})
            (self.sd("sleep") / name).unlink()
            return self.send_json(200, {})
        if path == "/api/wifi":
            state = self.state()
            state["wifi"] = {}
            self.save_state(state)
            return self.send_json(200, {})
        return self.send_json(404, {"error": "not found"})

    def info(self):
        if not self.authorized():
            return
        state = self.state()
        books = []
        for rel, path, book in self.books():
            pages = book.header.page_count
            pos = state["positions"].get(rel, 0)
            originals = self.sd("source").glob(path.stem + ".*")
            source = next((p.name for p in originals if p.suffix in (".epub", ".txt")), None)
            books.append({
                "file": rel,
                "folder": rel.rsplit("/", 1)[0] if "/" in rel else "",
                "title": book.header.title or path.name,
                "author": book.header.author,
                "pages": pages,
                "size": path.stat().st_size,
                "screen": self.book_screen(book).id,
                "position": pos,
                "progress": pos * 100 // (pages - 1) if pages > 1 else 0,
                "cover": book.cover is not None,
                "marks": len(self.read_marks(path.stem, pages)),
                "source": source,
            })  # fmt: skip
        books.sort(key=lambda b: b["title"].lower())
        total, free = self.card_space()
        font_files = sorted(self.sd("font").iterdir())
        fonts = [
            {"name": p.name, "size": p.stat().st_size}
            for p in font_files
            if p.suffix in KINDS["font"][1]
        ]
        sizes = picture_sizes(self.screen)
        pictures = [p.name for p in sorted(self.sd("sleep").glob("*.bin"))
                    if p.stat().st_size in sizes]  # fmt: skip
        sc = self.screen
        screen = {"id": sc.id, "width": sc.width, "height": sc.height, "statusBar": sc.status_bar,
                  "size": sc.font_size, "margin": sc.margin}  # fmt: skip
        stats = {"pages": 0, "seconds": 0, "daysRead": 0, "streak": 0, "bestStreak": 0,
                 "todayPages": 0, "todaySeconds": 0, "clock": "time" in state}  # fmt: skip
        self.send_json(200, {
            "version": "dev", "board": BOARD_FOR_SCREEN.get(sc.id, sc.id),
            "lang": "nl" if state["settings"].get("language") == 1 else "en",
            "name": state["name"], "homeWifi": state["wifi"].get("ssid", ""), "locked": False,
            "screen": screen, "battery": -1, "total": total, "free": free,
            "stats": stats, "books": books, "fonts": fonts, "pictures": pictures,
            **({"updateSite": self.update_site} if self.update_site else {}),
        })  # fmt: skip

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

    def book_dir_for(self, name: str) -> Path | None:
        """bookDirFor() in transfer.cpp: the named folder, else where a book of that
        name already is, else /books."""
        q = self.query()
        root = self.sd("book")
        if "folder" in q:
            folder = q["folder"]
            if not folder:
                return root
            return root / folder if is_safe_folder(folder) else None
        for _, path, _ in self.books():
            if path.name == name:
                return path.parent
        return root

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
        page_bytes = stride_for(self.screen.width) * self.screen.height
        if kind == "book":  # as kBookMaxBytes in transfer.cpp: 64 MB or 4096 pages
            limit = max(limit, 4096 * page_bytes)
        if kind == "sleep":
            limit = page_bytes
        if len(name.encode()) > 64:
            return self.send_json(400, {"error": "file name too long (64 bytes at most)"})
        if not is_safe_name(name, exts):
            return self.send_json(400, {"error": f"only {'/'.join(exts)} files with a plain name"})
        # as transfer.cpp: the whole request must fit next to a book it replaces
        free = self.card_space()[1]
        if int(self.headers.get("Content-Length", 0)) > free:
            return self.send_json(507, {"error": "not enough space on the SD card", "free": free})
        if len(data) > limit:
            return self.send_json(413, {"error": "file too large"})
        sizes = picture_sizes(self.screen)
        if kind == "sleep" and len(data) not in sizes:
            sc, side = self.screen, min(self.screen.width, self.screen.height)
            why = (f"a sleep picture is {sc.width}x{sc.height} ({sizes[0]} bytes) "
                   f"or {side}x{side} ({sizes[1]} bytes)")  # fmt: skip
            return self.send_json(400, {"error": why})
        folder = self.book_dir_for(name) if kind == "book" else self.sd(kind)
        if folder is None:
            return self.send_json(400, {"error": "bad folder name"})
        folder.mkdir(parents=True, exist_ok=True)
        dest = folder / name
        tmp = dest.with_name(dest.name + ".part")
        tmp.write_bytes(data)
        title = name
        if kind == "book":
            try:
                book = T5BFile(tmp)
                if not self.book_screen(book):
                    raise ValueError("made for another screen size")
                title = book.header.title or name
            except ValueError as e:
                tmp.unlink()
                return self.send_json(400, {"error": str(e)})
            if dest.exists():  # replacing a book (restyled): positions move along
                self.remap_positions(dest, book.header.page_count)
        tmp.replace(dest)
        return self.send_json(200, {"ok": 1, "title": title, "free": self.card_space()[1]})

    def remap_positions(self, dest: Path, new: int) -> None:
        with contextlib.suppress(ValueError):
            old = T5BFile(dest).header.page_count
            state = self.state()
            rel = dest.relative_to(self.sd("book")).as_posix()
            if rel in state["positions"]:
                state["positions"][rel] = remap_page(state["positions"][rel], old, new)
                self.save_state(state)
            marks = self.read_marks(dest.stem, old)
            self.write_marks(dest.stem, [remap_page(m, old, new) for m in marks])

    def move(self):
        if not self.authorized():
            return None
        got = self.book_arg()
        if not got:
            return None
        rel, path, _ = got
        folder = self.query().get("folder", "")
        if folder and not is_safe_folder(folder):
            return self.send_json(400, {"error": "bad name"})
        dest_dir = self.sd("book") / folder if folder else self.sd("book")
        dest = dest_dir / path.name
        if dest == path:
            return self.send_json(200, {})
        if dest.exists():
            return self.send_json(409, {"error": "a book with that name is already there"})
        dest_dir.mkdir(parents=True, exist_ok=True)
        path.replace(dest)
        state = self.state()
        if rel in state["positions"]:
            state["positions"][dest.relative_to(self.sd("book")).as_posix()] = state[
                "positions"
            ].pop(rel)
            self.save_state(state)
        if path.parent != self.sd("book"):
            with contextlib.suppress(OSError):
                path.parent.rmdir()
        return self.send_json(200, {})

    def marks(self):
        if not self.authorized():
            return None
        got = self.book_arg()
        if not got:
            return None
        _, path, book = got
        pages = book.header.page_count
        marks = self.read_marks(path.stem, pages)
        if self.command == "POST":
            q = self.query()
            page = int(q["page"]) if q.get("page", "").isdigit() else -1
            if not 0 <= page < pages:
                return self.send_json(400, {"error": "bad page"})
            if q.get("op") == "remove":
                marks = [m for m in marks if m != page]
            elif page not in marks:
                marks.append(page)
            self.write_marks(path.stem, marks)
            marks = self.read_marks(path.stem, pages)
        return self.send_json(200, {"pages": marks})

    def position(self):
        if not self.authorized():
            return None
        got = self.book_arg()
        if not got:
            return None
        rel, _, book = got
        page = self.query().get("page", "")
        if not page.isdigit() or int(page) >= book.header.page_count:
            return self.send_json(400, {"error": "bad page"})
        state = self.state()
        state["positions"][rel] = int(page)
        self.save_state(state)
        return self.send_json(200, {})

    def page(self):
        if not self.authorized():
            return None
        got = self.book_arg()
        if not got:
            return None
        _, _, book = got
        n = self.query().get("n", "")
        if not n.isdigit() or int(n) >= book.header.page_count:
            return self.send_json(400, {"error": "bad page"})
        data = book.page_data(int(n))
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        return None

    def settings(self):
        if not self.authorized():
            return None
        state = self.state()
        if self.command == "POST":
            q = self.query()
            options = {k: len(o) for k, _, o, _ in settings_for(self.screen)}
            key, value = q.get("key", ""), q.get("value", "")
            if key not in options or not value.isdigit() or int(value) >= options[key]:
                return self.send_json(400, {"error": "bad setting"})
            state["settings"][key] = int(value)
            self.save_state(state)
        return self.send_json(200, {"settings": [
            {"key": k, "label": label, "value": state["settings"].get(k, d), "options": opts}
            for k, label, opts, d in settings_for(self.screen)
        ]})  # fmt: skip

    def name(self):
        if not self.authorized():
            return None
        name = self.query().get("name", "")
        if len(name.encode()) > 32 or any(ord(c) < 0x20 for c in name):
            return self.send_json(400, {"error": "bad name"})
        state = self.state()
        state["name"] = name
        self.save_state(state)
        return self.send_json(200, {})

    def wifi(self):
        if not self.authorized():
            return None
        q = self.query()
        ssid, password = q.get("ssid", ""), q.get("password", "")
        if not 1 <= len(ssid.encode()) <= 32 or len(password.encode()) > 63:
            return self.send_json(
                400, {"error": "network name 1-32 characters, password at most 63"}
            )
        state = self.state()
        state["wifi"] = {"ssid": ssid}  # the dev server never keeps the password
        self.save_state(state)
        return self.send_json(200, {})

    def set_time(self):
        if not self.authorized():
            return None
        q = self.query()
        try:
            epoch, tz = int(q.get("epoch", "")), int(q.get("tz", ""))
        except ValueError:
            return self.send_json(400, {"error": "bad time"})
        if epoch < 1700000000 or abs(tz) > 14 * 3600:
            return self.send_json(400, {"error": "bad time"})
        state = self.state()
        state["time"] = {"epoch": epoch, "tz": tz, "at": time.time()}
        self.save_state(state)
        return self.send_json(200, {})

    def reset(self):
        if not self.authorized():
            return None
        if self.query().get("confirm") != "erase":
            return self.send_json(400, {"error": "confirm=erase missing"})
        for sub in ("books", "sleep", "fonts"):
            shutil.rmtree(self.card() / sub, ignore_errors=True)
        (self.root / "state.json").unlink(missing_ok=True)
        return self.send_json(200, {})

    def get_file(self):
        if not self.authorized():
            return None
        q = self.query()
        kind, name = q.get("kind", ""), q.get("name", "")
        allowed = ("source", "meta", "font", "sleep")
        if kind not in allowed or not is_safe_name(name, KINDS[kind][1]):
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
    root: Path,
    code: str,
    host: str = "127.0.0.1",
    port: int = 8000,
    fonts: bool = True,
    screen: str = DEFAULT_SCREEN,
    card_mb: float | None = None,
    update_site: str | None = None,
) -> ThreadingHTTPServer:
    root.mkdir(parents=True, exist_ok=True)
    write_files(build_assets(Path("firmware/.pio/fontcache"), fonts=fonts), root / "www")

    class Bound(Handler):
        pass

    Bound.root = root
    Bound.code = code
    Bound.screen = SCREENS[screen]
    Bound.card_bytes = int(card_mb * 1048576) if card_mb else None
    Bound.update_site = update_site

    def factory(*args, **kwargs):
        return Bound(*args, directory=str(root / "www"), **kwargs)

    return ThreadingHTTPServer((host, port), factory)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="web-dev", description=__doc__.splitlines()[0])
    p.add_argument("--root", type=Path, default=Path("out/web-dev"), help="assets + fake SD card")
    p.add_argument("--port", type=int, default=8000)
    p.add_argument("--lan", action="store_true", help="listen on all interfaces (phone access)")
    p.add_argument("--code", default=None, help="session code (default: random)")
    p.add_argument("--screen", choices=sorted(SCREENS), default=DEFAULT_SCREEN, help="reader")
    p.add_argument("--card-mb", type=float, help="fake card size (try a full card)")
    p.add_argument("--update-site", help="where the page looks for releases (default: GitHub)")
    args = p.parse_args(argv)
    code = args.code or f"{random.randrange(10000):04d}"
    bind = "0.0.0.0" if args.lan else "127.0.0.1"
    server = make_server(
        args.root, code, bind, args.port, screen=args.screen, card_mb=args.card_mb,
        update_site=args.update_site,
    )  # fmt: skip
    host = lan_address() if args.lan else "localhost"
    print(f"T5 reader dev server: http://{host}:{args.port}/?k={code}   (code {code})")
    print(f"screen: {SCREENS[args.screen].name}")
    print(f"fake SD card: {args.root / 'sd'}   Ctrl+C to stop")
    with contextlib.suppress(KeyboardInterrupt):
        server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
