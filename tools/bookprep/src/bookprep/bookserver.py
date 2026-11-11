"""Book server: a folder of EPUB/TXT books, typeset on request for the reader that
asks, per profile (screen, font, size, ...), and downloaded by the reader itself over
Wi-Fi (Menu -> Book server). Runs as a Home Assistant add-on (homeassistant/t5books)
or on any PC:

    pixi run book-server --library ~/Books                  # http://localhost:8484
    pixi run book-server --options profiles.json            # the add-on's options

Options (the add-on's /data/options.json; --library overrides "library"):

    {"library": "/share/calibre-dedrm/books",
     "profiles": [{"name": "Literata 18", "screen": "7.5in", "font": "Literata", "size": 18}],
     "prepare_newest": 5, "cache_mb": 2000}

A screen without a profile gets a default one (bookprep's defaults for it). Books are
converted by `bookprep` (one at a time, at low priority) and kept in the cache, a
folder per profile, until the cache is full (least recently used go first).

API for the reader (firmware/src/bookserver.cpp): plain UTF-8 text, a header line
"T5BOOKS<TAB>1", then one record per line, fields separated by tabs (the reader has
no JSON parser). Page numbers are 1-based.

    GET /api/profiles?screen=<id>            profile <id> <name>
    GET /api/books?sort=new|title[&author=<id>]&offset=&count=
                                             total <n>, then book <id> <title> <author> <file>
    GET /api/authors?offset=&count=          total <n>, then author <id> <name> <books>
    GET /api/prepare?profile=&book=[&room=<bytes>]
                                             busy <percent> <jobs ahead> | error <message> |
                                             ready <pages> <bytes> <file>, and if it is bigger
                                             than room: part <first> <last> <bytes> for parts
                                             that fit, one after the other (none: no room)
    GET /api/download?profile=&book=[&first=&last=]   the .t5b (or that part of it)

Plus / (a page for people: books, profiles, downloads in a browser) and /health.
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import html
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass, field
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, quote, urlparse

from . import __version__
from .layout import SCREENS, line_height_for
from .library import Book, Library, slug
from .t5b import T5BFile, part_size, plan_parts, write_part

PROTOCOL = "T5BOOKS\t1"
DEFAULT_PORT = 8484
FONT_EXTS = (".ttf", ".otf")
TEXT_FIELD_BYTES = 90  # the reader's list rows hold 95 bytes
LIST_MAX = 200
RETRY_FAILED_AFTER = 600  # seconds before a failed conversion is tried again


def code_id() -> str:
    """A hash of bookprep's own code that shapes a book: a change typesets them again,
    even under the same version (a local add-on rebuilt from your working copy)."""
    here = Path(__file__).parent
    h = hashlib.sha1()
    for name in ("cli", "extract", "layout", "normalize", "render", "t5b"):
        h.update((here / f"{name}.py").read_bytes())
    return h.hexdigest()[:12]


def log(*parts) -> None:
    print(time.strftime("%H:%M:%S"), *parts, flush=True)


# --- profiles ----------------------------------------------------------------------------


@dataclass(frozen=True)
class Profile:
    """How books are typeset for one reader: a screen and bookprep's options."""

    id: str
    name: str
    screen: str
    font: str = ""  # a bundled font's name, a .ttf/.otf in the font folders, or a path
    size: int | None = None  # px; None: the screen's default
    line_spacing: float | None = None  # line height = size x this (default 1.2)
    margin: int | None = None
    indent: int | None = None  # first-line indent in px (default: the size)
    paragraph_spacing: int = 0
    justify: bool = True
    hyphenate: bool = True
    language: str = ""  # hyphenation language; "" = the book's own
    weight: int = 128  # grey level that turns black: 70 lighter ... 210 bolder
    bionic: bool = False
    cover: bool = True
    chapter_break: bool = True

    def argv(self, font_path: Path) -> list[str]:
        """bookprep's options for this profile (`pixi run prep book.epub <these>`)."""
        out = ["--screen", self.screen, "--font", str(font_path), "--threshold", str(self.weight)]
        if self.size:
            out += ["--size", str(self.size)]
        if self.line_spacing:
            size = self.size or SCREENS[self.screen].font_size
            out += ["--line-height", str(line_height_for(size, self.line_spacing))]
        for flag, value in (("--margin", self.margin), ("--indent", self.indent)):
            if value is not None:
                out += [flag, str(value)]
        if self.paragraph_spacing:
            out += ["--paragraph-spacing", str(self.paragraph_spacing)]
        if self.language:
            out += ["--lang", self.language]
        for flag, on in (
            ("--no-justify", not self.justify),
            ("--no-hyphenate", not self.hyphenate),
            ("--bionic", self.bionic),
            ("--no-cover", not self.cover),
            ("--no-chapter-break", not self.chapter_break),
        ):
            if on:
                out.append(flag)
        return out

    def summary(self) -> str:
        size = self.size or SCREENS[self.screen].font_size
        return f"{SCREENS[self.screen].name}, {self.font or 'Ubuntu'} {size} px"


PROFILE_FIELDS = {
    "font": str, "size": int, "line_spacing": float, "margin": int, "indent": int,
    "paragraph_spacing": int, "justify": bool, "hyphenate": bool, "language": str,
    "weight": int, "bionic": bool, "cover": bool, "chapter_break": bool,
}  # fmt: skip


def parse_profiles(items: list[dict]) -> list[Profile]:
    """Profiles from the options; a bad one is reported and left out."""
    out: list[Profile] = []
    ids: set[str] = set()
    for n, item in enumerate(items, 1):
        name = str(item.get("name") or "").strip()
        screen = str(item.get("screen") or "")
        if not name or screen not in SCREENS:
            log(f"profile {n}: needs a name and a screen ({', '.join(SCREENS)}); left out")
            continue
        kwargs = {}
        for key, kind in PROFILE_FIELDS.items():
            value = item.get(key)
            if value is None or value == "":
                continue
            try:
                kwargs[key] = kind(value) if kind is not bool else bool(value)
            except (TypeError, ValueError):
                log(f"profile {name!r}: {key} {value!r} is not a {kind.__name__}; ignored")
        base = slug(name).lower()[:24] or "profile"
        pid, i = base, 2
        while pid in ids:
            pid, i = f"{base}-{i}", i + 1
        ids.add(pid)
        out.append(Profile(pid, name, screen, **kwargs))
    return out


def default_profile(screen: str) -> Profile:
    return Profile(f"default-{screen}", "Default", screen)


# --- fonts ---------------------------------------------------------------------------------


def _norm(name: str) -> str:
    return "".join(c for c in name.casefold() if c.isalnum())


class Fonts:
    """Font names in profiles -> TTF files. The page's bundled fonts (webassets.FONTS)
    by name, as static TTFs at the page's axes (made once into `cache`; the add-on's
    image has them already), else .ttf/.otf files in the font folders, else a path."""

    def __init__(self, dirs: list[Path], cache: Path):
        self.dirs = [Path(d) for d in dirs]
        self.cache = Path(cache)
        self._digests: dict[tuple[str, int], str] = {}

    def resolve(self, name: str) -> Path:
        from .webassets import FONTS

        key = _norm(name or "ubuntu")
        for spec in FONTS:
            if key in (_norm(spec.id), _norm(spec.name)):
                return self.bundled(spec)
        for d in self.dirs:
            if d.is_dir():
                for f in sorted(d.iterdir()):
                    if f.suffix.lower() in FONT_EXTS and key in (_norm(f.name), _norm(f.stem)):
                        return f
        path = Path(name)
        if path.is_file():
            return path
        bundled = ", ".join(s.name for s in FONTS)
        folders = ", ".join(str(d) for d in self.dirs) or "a font folder"
        raise LookupError(f"font {name!r} not found (bundled: {bundled}; or a .ttf in {folders})")

    def bundled(self, spec) -> Path:
        from fontTools.ttLib import TTFont
        from fontTools.varLib.instancer import instantiateVariableFont

        from .webassets import font_source

        if spec.source.startswith("env:"):
            return font_source(spec, self.cache)  # a TTF already (pixi env, or downloaded)
        out = self.cache / f"{spec.id}.ttf"
        if not out.exists():
            font = TTFont(str(font_source(spec, self.cache)))
            if spec.axes:
                font = instantiateVariableFont(font, dict(spec.axes))
            font.flavor = None  # Pillow's FreeType reads no WOFF2
            self.cache.mkdir(parents=True, exist_ok=True)
            tmp = out.with_suffix(".tmp")
            font.save(str(tmp))
            tmp.replace(out)
        return out

    def digest(self, path: Path) -> str:
        """Content hash of a font file (cached by size and time)."""
        st = path.stat()
        key = (str(path), st.st_mtime_ns)
        if key not in self._digests:
            self._digests[key] = hashlib.sha256(path.read_bytes()).hexdigest()[:16]
        return self._digests[key]


def fetch_fonts(cache: Path) -> None:
    """Every bundled font as a TTF into `cache` (the add-on does this when it is built)."""
    from .webassets import FONTS

    fonts = Fonts([], cache)
    for spec in FONTS:
        print(f"{spec.name}: {fonts.bundled(spec)}")


# --- converted books -------------------------------------------------------------------------


@dataclass
class Job:
    key: str
    book: Book
    profile: Profile
    argv: list[str]
    out: Path
    state: str = "queued"  # queued, running, done, failed
    progress: float = 0.0
    error: str = ""
    finished: float = 0.0


@dataclass
class Status:
    state: str  # ready, busy, error
    path: Path | None = None
    progress: float = 0.0
    ahead: int = 0
    error: str = ""


class Converter:
    """Converts books with bookprep, one at a time, into the cache:
    <cache>/<profile key>/<book version>.t5b. The profile key covers bookprep's
    options, the font file and bookprep's code, so a change converts again."""

    def __init__(self, cache: Path, fonts: Fonts, max_bytes: int, nice: int = 10):
        self.cache = Path(cache)
        self.fonts = fonts
        self.max_bytes = max_bytes
        self.nice = nice
        self.version = os.environ.get("T5BOOKS_VERSION", __version__)
        self.code = code_id()
        self._lock = threading.Condition()
        self._jobs: dict[str, Job] = {}
        self._queue: list[Job] = []
        self._stopped = False
        self._thread = threading.Thread(target=self._work, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        with self._lock:
            self._stopped = True
            self._lock.notify_all()

    def profile_key(self, profile: Profile) -> tuple[str, list[str]]:
        font = self.fonts.resolve(profile.font)
        argv = profile.argv(font)
        digest = self.fonts.digest(font)  # the font's contents, not where it is
        ident = json.dumps([self.code, *(digest if a == str(font) else a for a in argv)])
        return hashlib.sha1(ident.encode()).hexdigest()[:16], argv

    def path(self, profile_key: str, book: Book) -> Path:
        return self.cache / profile_key / f"{book.version}.t5b"

    def status(self, book: Book, profile: Profile, urgent: bool = True) -> Status:
        """Ready (and where), or queued/running (how far), or failed (why). Not ready
        yet: the conversion is queued, first in line when urgent (a reader waits)."""
        try:
            pkey, argv = self.profile_key(profile)
        except (LookupError, OSError, ValueError) as e:
            return Status("error", error=str(e))
        out = self.path(pkey, book)
        if out.exists():
            with contextlib.suppress(OSError):
                os.utime(out)  # recently used: kept longest
            return Status("ready", path=out)
        key = f"{pkey}/{book.version}"
        with self._lock:
            job = self._jobs.get(key)
            recent = job is not None and time.time() - job.finished < RETRY_FAILED_AFTER
            if job and job.state == "failed" and (recent or not urgent):
                return Status("error", error=job.error)
            if job is None or job.state in ("failed", "done"):
                job = Job(key, book, profile, argv, out)
                self._jobs[key] = job
                self._queue.insert(0 if urgent else len(self._queue), job)
                self._lock.notify_all()
                if urgent:
                    log(f"queued {book.rel} for {profile.name}")
            elif urgent and job in self._queue and self._queue[0] is not job:
                self._queue.remove(job)
                self._queue.insert(0, job)
            ahead = self._queue.index(job) + self._running() if job in self._queue else 0
            return Status("busy", progress=job.progress, ahead=ahead)

    def _running(self) -> int:
        return sum(1 for j in self._jobs.values() if j.state == "running")

    def jobs(self) -> list[Job]:
        with self._lock:
            return [j for j in self._jobs.values() if j.state in ("running", "queued")]

    def _work(self) -> None:
        while True:
            with self._lock:
                while not self._queue and not self._stopped:
                    self._lock.wait()
                if self._stopped:
                    return
                job = self._queue.pop(0)
                job.state = "running"
            self._run(job)

    def _run(self, job: Job) -> None:
        start = time.monotonic()
        log(f"converting {job.book.rel} for {job.profile.name}")
        tmp = Path(tempfile.mkdtemp(prefix="convert-", dir=self._tmp_root()))
        cmd = [sys.executable, "-m", "bookprep.cli", str(job.book.path), *job.argv]
        cmd += ["--out", str(tmp), "--progress"]
        tail: list[str] = []
        try:
            proc = subprocess.Popen(
                cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                preexec_fn=(lambda: os.nice(self.nice)) if hasattr(os, "nice") else None,
            )  # fmt: skip
            assert proc.stdout is not None
            for line in proc.stdout:
                if line.startswith("progress "):
                    with contextlib.suppress(ValueError):
                        job.progress = float(line.split()[1])
                elif line.strip():
                    tail = [*tail[-19:], line.rstrip()]
            code = proc.wait()
            made = list(tmp.glob("*.t5b"))
            if code != 0 or len(made) != 1:
                errors = [t for t in tail if t.startswith("error")]
                raise RuntimeError(errors[-1] if errors else (tail or [f"exit code {code}"])[-1])
            T5BFile(made[0])  # a complete, valid book
            job.out.parent.mkdir(parents=True, exist_ok=True)
            os.replace(made[0], job.out)
            with self._lock:
                job.state, job.progress, job.finished = "done", 1.0, time.time()
            log(f"done {job.book.rel} for {job.profile.name}: {time.monotonic() - start:.0f} s, "
                f"{job.out.stat().st_size // 1024} KB")  # fmt: skip
            self.prune()
        except Exception as e:  # a bad book must not stop the worker
            with self._lock:
                job.state, job.finished = "failed", time.time()
                job.error = str(e) or type(e).__name__
            log(f"failed {job.book.rel} for {job.profile.name}: {job.error}")
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def _tmp_root(self) -> Path:
        root = self.cache / "tmp"
        root.mkdir(parents=True, exist_ok=True)
        return root

    def prune(self, keep: set[str] | None = None) -> None:
        """Drops profile folders not in `keep` (when given), then the least recently
        used books until the cache fits in max_bytes."""
        if not self.cache.is_dir():
            return
        if keep is not None:
            for d in self.cache.iterdir():
                if d.is_dir() and d.name not in keep:
                    shutil.rmtree(d, ignore_errors=True)
        files = sorted(
            (p for p in self.cache.glob("*/*.t5b") if p.parent.name != "tmp"),
            key=lambda p: p.stat().st_mtime,
        )
        total = sum(p.stat().st_size for p in files)
        for p in files:
            if total <= self.max_bytes:
                break
            total -= p.stat().st_size
            p.unlink(missing_ok=True)
            log(f"cache full: dropped {p.parent.name}/{p.name}")


# --- the server -----------------------------------------------------------------------------


@dataclass
class App:
    library: Library
    profiles: list[Profile]
    converter: Converter
    prepare_newest: int = 0
    started: float = field(default_factory=time.time)

    def profiles_for(self, screen: str) -> list[Profile]:
        mine = [p for p in self.profiles if p.screen == screen]
        return mine or ([default_profile(screen)] if screen in SCREENS else [])

    def profile(self, pid: str) -> Profile | None:
        for p in self.profiles:
            if p.id == pid:
                return p
        if pid.startswith("default-") and pid[8:] in SCREENS:
            return default_profile(pid[8:])
        return None

    def all_profiles(self) -> list[Profile]:
        """Configured profiles, plus the default one of each screen without any."""
        have = {p.screen for p in self.profiles}
        return self.profiles + [default_profile(s) for s in SCREENS if s not in have]

    def prepare_in_background(self) -> None:
        """Keeps the newest books converted for every configured profile."""
        while self.prepare_newest > 0:
            for book in self.library.newest()[: self.prepare_newest]:
                for profile in self.profiles:
                    self.converter.status(book, profile, urgent=False)
            time.sleep(60)


def fit(text: str, limit: int = TEXT_FIELD_BYTES) -> str:
    """One protocol field: no tabs or line breaks, at most `limit` bytes of UTF-8."""
    text = " ".join(str(text).replace("\t", " ").split())
    data = text.encode("utf-8")[:limit]
    return data.decode("utf-8", "ignore")


def int_arg(q: dict, name: str, default: int, lo: int = 0, hi: int = 1 << 62) -> int:
    try:
        return max(lo, min(hi, int(q.get(name, [default])[0])))
    except ValueError:
        return default


class Handler(BaseHTTPRequestHandler):
    server_version = f"T5BookServer/{__version__}"
    app: App  # set on the subclass by make_server()

    def log_message(self, fmt, *args) -> None:
        pass  # requests are logged where they matter

    # --- replies ---

    def send(self, code: int, body: bytes, mime: str, headers: dict | None = None) -> None:
        self.send_response(code)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def lines(self, rows: list[list], code: int = 200) -> None:
        text = PROTOCOL + "\n" + "".join("\t".join(fit(f) for f in r) + "\n" for r in rows)
        self.send(code, text.encode("utf-8"), "text/plain; charset=utf-8")

    def fail(self, code: int, message: str) -> None:
        self.lines([["error", message]], code)

    def send_file(self, path: Path, name: str) -> None:
        size = path.stat().st_size
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(size))
        self.send_header("Content-Disposition", f'attachment; filename="{name}"')
        self.end_headers()
        with open(path, "rb") as f:
            shutil.copyfileobj(f, self.wfile, 64 * 1024)

    # --- routing ---

    def do_GET(self) -> None:
        url = urlparse(self.path)
        q = parse_qs(url.query)
        routes = {
            "/api/profiles": self.api_profiles,
            "/api/books": self.api_books,
            "/api/authors": self.api_authors,
            "/api/prepare": self.api_prepare,
            "/api/download": self.api_download,
            "/health": lambda q: self.send(200, b"ok\n", "text/plain"),
            "/": self.page,
            "/get": self.page_get,
        }
        route = routes.get(url.path)
        if route is None:
            return self.fail(404, "not found")
        with contextlib.suppress(BrokenPipeError, ConnectionResetError):  # the reader left
            route(q)

    def arg(self, q: dict, name: str) -> str:
        return q.get(name, [""])[0]

    def book_and_profile(self, q: dict) -> tuple[Book, Profile] | None:
        profile = self.app.profile(self.arg(q, "profile"))
        book = self.app.library.get(self.arg(q, "book"))
        if profile is None:
            self.fail(404, "no such profile (changed in the add-on?)")
        elif book is None:
            self.fail(404, "no such book (moved or deleted?)")
        else:
            return book, profile
        return None

    # --- the reader's API ---

    def api_profiles(self, q: dict) -> None:
        screen = self.arg(q, "screen")
        if screen not in SCREENS:
            return self.fail(400, f"unknown screen {screen!r}")
        log(f"reader {self.client_address[0]} ({screen}) connected")
        self.lines([["profile", p.id, p.name] for p in self.app.profiles_for(screen)])

    def api_books(self, q: dict) -> None:
        lib = self.app.library
        author_id = self.arg(q, "author")
        if author_id:
            author = lib.author(author_id)
            if author is None:
                return self.fail(404, "no such author")
            books = list(author.books)
        else:
            books = lib.by_title() if self.arg(q, "sort") == "title" else lib.newest()
        offset = int_arg(q, "offset", 0)
        count = int_arg(q, "count", 50, 1, LIST_MAX)
        rows = [["total", len(books)]]
        rows += [
            ["book", b.id, b.title, b.author, b.file_name] for b in books[offset : offset + count]
        ]
        self.lines(rows)

    def api_authors(self, q: dict) -> None:
        authors = self.app.library.authors()
        offset = int_arg(q, "offset", 0)
        count = int_arg(q, "count", 50, 1, LIST_MAX)
        rows = [["total", len(authors)]]
        rows += [["author", a.id, a.name, len(a.books)] for a in authors[offset : offset + count]]
        self.lines(rows)

    def api_prepare(self, q: dict) -> None:
        found = self.book_and_profile(q)
        if found is None:
            return
        book, profile = found
        st = self.app.converter.status(book, profile)
        if st.state == "error":
            return self.lines([["error", st.error]])
        if st.state == "busy":
            return self.lines([["busy", int(st.progress * 100), st.ahead]])
        assert st.path is not None
        t5b = T5BFile(st.path)
        size = st.path.stat().st_size
        rows = [["ready", t5b.header.page_count, size, book.file_name]]
        room = int_arg(q, "room", 0)
        if room and size > room:
            rows += [["part", a + 1, b, part_size(t5b, a, b)] for a, b in plan_parts(t5b, room)]
        self.lines(rows)

    def api_download(self, q: dict) -> None:
        found = self.book_and_profile(q)
        if found is None:
            return
        book, profile = found
        st = self.app.converter.status(book, profile)
        if st.state != "ready" or st.path is None:
            return self.fail(409, st.error or "not converted yet: ask /api/prepare first")
        if "first" not in q and "last" not in q:
            log(f"reader {self.client_address[0]}: {book.rel} ({profile.name})")
            return self.send_file(st.path, book.file_name)
        t5b = T5BFile(st.path)
        first = int_arg(q, "first", 1, 1)
        last = int_arg(q, "last", t5b.header.page_count, 1)
        if not first <= last <= t5b.header.page_count:
            return self.fail(400, f"pages {first}-{last}: the book has {t5b.header.page_count}")
        log(f"reader {self.client_address[0]}: {book.rel} pages {first}-{last} ({profile.name})")
        with tempfile.TemporaryDirectory(dir=self.app.converter._tmp_root()) as tmp:
            part = Path(tmp) / book.file_name
            write_part(t5b, part, first - 1, last)
            self.send_file(part, book.file_name)

    # --- a page for people ---

    def page(self, q: dict) -> None:
        app = self.app
        books = app.library.newest()
        profiles = app.all_profiles()
        e = html.escape
        reader_url = f"http://{self.headers.get('Host') or 'this-address:8484'}"
        rows = []
        for b in books:
            links = " ".join(
                f'<a href="/get?profile={quote(p.id)}&amp;book={b.id}" title="{e(p.summary())}">'
                f"{e(p.name if p.id != f'default-{p.screen}' else SCREENS[p.screen].name)}</a>"
                for p in profiles
            )
            added = datetime.fromtimestamp(b.added).strftime("%Y-%m-%d")
            rows.append(f"<tr><td>{e(b.title)}<br><small>{e(b.author)}</small></td>"
                        f"<td>{added}</td><td>{links}</td></tr>")  # fmt: skip
        jobs = "".join(
            f"<li>{e(j.book.title)} ({e(j.profile.name)}): "
            f"{f'{j.progress:.0%}' if j.state == 'running' else 'waiting'}</li>"
            for j in app.converter.jobs()
        )
        prof = "".join(
            f"<li><b>{e(p.name)}</b>: {e(p.summary())}"
            f"{' (automatic)' if p.id.startswith('default-') else ''}</li>"
            for p in profiles
        )
        body = f"""<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>T5 book server</title>
<style>body{{font:15px/1.4 system-ui,sans-serif;max-width:900px;margin:24px auto;padding:0 16px;
color:#222;background:#fafaf7}}h1{{font-size:22px}}h2{{font-size:16px;margin-top:28px}}
table{{border-collapse:collapse;width:100%}}td{{padding:6px 8px;border-top:1px solid #ddd;
vertical-align:top}}small{{color:#666}}a{{margin-right:8px}}code{{background:#eee;padding:0 4px}}
@media(prefers-color-scheme:dark){{body{{background:#1b1b1a;color:#ddd}}td{{border-color:#444}}
small{{color:#999}}a{{color:#9cf}}code{{background:#333}}}}</style></head><body>
<h1>T5 book server</h1>
<p>{len(books)} books in <code>{e(str(app.library.root))}</code>.</p>
<p>On the reader: Menu &rarr; <b>Book server</b>. Its address goes on the reader's page
(Wi-Fi transfer &rarr; Settings &rarr; Book server): <code>{e(reader_url)}</code>.
Here: download a book typeset for a profile, to send it from the reader's page.</p>
<h2>Profiles</h2><ul>{prof}</ul>
{f"<h2>Converting</h2><ul>{jobs}</ul>" if jobs else ""}
<h2>Books, newest first</h2><table>{"".join(rows)}</table>
<p><small>bookprep {e(app.converter.version)}</small></p></body></html>"""
        self.send(200, body.encode("utf-8"), "text/html; charset=utf-8")

    def page_get(self, q: dict) -> None:
        found = self.book_and_profile(q)
        if found is None:
            return
        book, profile = found
        st = self.app.converter.status(book, profile)
        if st.state == "ready" and st.path is not None:
            return self.send_file(st.path, book.file_name)
        e = html.escape
        if st.state == "error":
            text, refresh = f"Could not convert it: {e(st.error)}", ""
        else:
            ahead = f", {st.ahead} ahead" if st.ahead else ""
            text = f"Typesetting for {e(profile.name)}: {int(st.progress * 100)}%{ahead}"
            refresh = '<meta http-equiv="refresh" content="3">'
        body = (f'<!doctype html><meta charset="utf-8">{refresh}<title>{e(book.title)}</title>'
                f'<body style="font:16px system-ui;margin:40px"><h2>{e(book.title)}</h2>'
                f'<p>{text}</p><p><a href="/">Back</a></p>')  # fmt: skip
        self.send(200, body.encode("utf-8"), "text/html; charset=utf-8")


def make_server(app: App, host: str = "0.0.0.0", port: int = DEFAULT_PORT) -> ThreadingHTTPServer:
    handler = type("BoundHandler", (Handler,), {"app": app})
    server = ThreadingHTTPServer((host, port), handler)
    server.daemon_threads = True
    return server


def load_options(path: Path | None) -> dict:
    if path is None:
        return {}
    return json.loads(Path(path).read_text(encoding="utf-8"))


def build_app(options: dict, cache: Path, font_cache: Path, font_dirs: list[Path]) -> App:
    library = Library(Path(options.get("library") or "."), log=log)
    profiles = parse_profiles(options.get("profiles") or [])
    fonts = Fonts(font_dirs, font_cache)
    max_bytes = int(options.get("cache_mb") or 2000) << 20
    converter = Converter(cache, fonts, max_bytes)
    app = App(library, profiles, converter, int(options.get("prepare_newest") or 0))
    keep = {"tmp"}
    for p in app.all_profiles():
        try:
            keep.add(converter.profile_key(p)[0])
            log(f"profile {p.name!r} ({p.id}): {p.summary()}")
        except (LookupError, OSError, ValueError) as e:
            log(f"profile {p.name!r}: {e}")
    converter.prune(keep)  # profiles that changed or went: their books go too
    return app


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="book-server", description=__doc__.split("\n\n")[0])
    p.add_argument("--options", type=Path, help="JSON options (the add-on's /data/options.json)")
    p.add_argument("--library", type=Path, help="folder with EPUB/TXT books")
    p.add_argument("--cache", type=Path, default=Path("out/book-server/cache"))
    p.add_argument("--font-cache", type=Path, default=Path("out/book-server/fonts"),
                   help="bundled fonts as TTF (made on first use)")  # fmt: skip
    p.add_argument("--fonts", type=Path, action="append", default=[],
                   help="folder with your own .ttf/.otf fonts (repeatable)")  # fmt: skip
    p.add_argument("--host", default="0.0.0.0")
    p.add_argument("--port", type=int, default=DEFAULT_PORT)
    p.add_argument("--fetch-fonts", type=Path, metavar="DIR",
                   help="only put every bundled font as TTF into DIR, then exit")  # fmt: skip
    args = p.parse_args(argv)
    if args.fetch_fonts:
        fetch_fonts(args.fetch_fonts)
        return 0
    options = load_options(args.options)
    if args.library:
        options["library"] = str(args.library)
    if not options.get("library"):
        p.error("no library: --library DIR, or 'library' in --options")
    app = build_app(options, args.cache, args.font_cache, args.fonts)
    books = app.library.scan()
    log(f"{len(books)} books in {app.library.root}")
    if not app.library.root.is_dir():
        log(f"warning: {app.library.root} is not a folder")
    threading.Thread(target=app.prepare_in_background, daemon=True).start()
    server = make_server(app, args.host, args.port)
    log(f"book server {app.converter.version} on http://{args.host}:{server.server_address[1]}/")
    with contextlib.suppress(KeyboardInterrupt):
        server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
