"""A folder of EPUB/TXT books for the book server: what is in it, and each book's
title, authors and when it was added.

Books are every .epub and .txt in the folder and its subfolders. Metadata comes from
Calibre's OPF beside the book (``<stem>.opf``, as ``calibre export --single-dir``
writes it, or ``metadata.opf`` in a Calibre library's book folder), else from the
EPUB's own OPF, else the file name. "Added" is Calibre's timestamp, else the file's
modification time.

A Calibre library *inside* the folder (a subfolder with a ``metadata.db``) is
skipped: its books are Calibre's copies of ones already there (the DeDRM add-on
exports every book to the folder and also adds it to such a library). To serve a
Calibre library itself, name its folder as the library.
"""

from __future__ import annotations

import contextlib
import hashlib
import os
import re
import threading
import time
import xml.etree.ElementTree as ET
import zipfile
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

BOOK_EXTS = (".epub", ".txt")
MAX_FILE_NAME = 64  # kMaxFileName in firmware/lib/core/src/web_util.h


def slug(name: str) -> str:
    """cli.slug() (bookprep.js: slug()): the name of a book on the reader."""
    return re.sub(r"[^A-Za-z0-9._-]+", "_", name).strip("_") or "book"


def reader_file_name(path: Path) -> str:
    """What the reader calls this book: as the web page names an uploaded one
    (fileNameFor() in firmware/web/index.html), so the two replace each other."""
    return slug(path.stem)[: MAX_FILE_NAME - 5] + ".t5b"


def short_id(text: str, n: int = 12) -> str:
    return hashlib.sha1(text.encode("utf-8")).hexdigest()[:n]


@dataclass(frozen=True)
class Book:
    id: str  # from the path below the library folder: stable while the file stays put
    path: Path
    rel: str
    title: str
    authors: tuple[str, ...]
    title_sort: str
    added: float  # seconds since the epoch
    size: int
    mtime_ns: int

    @property
    def author(self) -> str:
        return " & ".join(self.authors)

    @property
    def file_name(self) -> str:
        return reader_file_name(self.path)

    @property
    def version(self) -> str:
        """Changes when the file does (the converted copies depend on it)."""
        return short_id(f"{self.rel}\0{self.size}\0{self.mtime_ns}", 16)


@dataclass(frozen=True)
class Author:
    id: str
    name: str
    books: tuple[Book, ...]


def author_sort_key(name: str) -> tuple[str, str]:
    """Surname first: "Steven van de Vijver" under V, "Woolf, Virginia" as written."""
    name = name.strip()
    if "," in name:
        return (name.casefold(), name.casefold())
    words = name.split()
    return ((words[-1] if words else name).casefold(), name.casefold())


# --- metadata --------------------------------------------------------------------------


@dataclass
class Meta:
    title: str = ""
    authors: list[str] = field(default_factory=list)
    title_sort: str = ""
    added: float | None = None


def _local(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def parse_opf(data: bytes) -> Meta:
    """Title, authors, Calibre's title sort and timestamp from an OPF document."""
    meta = Meta()
    root = ET.fromstring(data)
    for el in root.iter():
        name = _local(el.tag) if isinstance(el.tag, str) else ""
        text = (el.text or "").strip()
        if name == "title" and text and not meta.title:
            meta.title = text
        elif name == "creator" and text:
            role = next((v for k, v in el.attrib.items() if _local(k) == "role"), "aut")
            if role == "aut":
                meta.authors += [a.strip() for a in text.split(" & ") if a.strip()]
        elif name == "meta":
            key, value = el.get("name", ""), el.get("content", "")
            if key == "calibre:title_sort":
                meta.title_sort = value
            elif key == "calibre:timestamp" and value:
                with contextlib.suppress(ValueError):
                    meta.added = datetime.fromisoformat(value).timestamp()
    return meta


def epub_opf(path: Path) -> bytes:
    """The package document inside an EPUB (META-INF/container.xml names it)."""
    with zipfile.ZipFile(path) as z:
        container = ET.fromstring(z.read("META-INF/container.xml"))
        for el in container.iter():
            if isinstance(el.tag, str) and _local(el.tag) == "rootfile" and el.get("full-path"):
                return z.read(el.get("full-path"))
    raise ValueError("no rootfile in META-INF/container.xml")


def read_meta(path: Path, single_in_folder: bool) -> Meta:
    """Metadata of one book: Calibre's OPF beside it, else the EPUB's own."""
    sources = [path.with_suffix(".opf")]
    if single_in_folder:
        sources.append(path.parent / "metadata.opf")
    for opf in sources:
        if opf.is_file():
            try:
                meta = parse_opf(opf.read_bytes())
                if meta.title:
                    return meta
            except (ET.ParseError, OSError):
                pass
    if path.suffix.lower() == ".epub":
        try:
            return parse_opf(epub_opf(path))
        except (ET.ParseError, OSError, KeyError, ValueError, zipfile.BadZipFile):
            pass
    return Meta()


# --- the library -----------------------------------------------------------------------


class Library:
    """The books in `root`, rescanned when older than `max_age` seconds. Metadata is
    read once per file version. Thread-safe."""

    def __init__(self, root: Path, max_age: float = 30.0, log=print):
        self.root = Path(root)
        self.max_age = max_age
        self.log = log
        self._lock = threading.Lock()
        self._meta: dict[str, tuple[int, int, Book]] = {}  # rel -> (size, mtime_ns, book)
        self._books: list[Book] = []
        self._by_id: dict[str, Book] = {}
        self._scanned = 0.0
        self._skipped: set[str] = set()

    def scan(self) -> list[Book]:
        with self._lock:
            if time.monotonic() - self._scanned < self.max_age and self._scanned:
                return self._books
            books = []
            for path, single in self._files():
                rel = path.relative_to(self.root).as_posix()
                try:
                    st = path.stat()
                except OSError:
                    continue
                known = self._meta.get(rel)
                if known and known[:2] == (st.st_size, st.st_mtime_ns):
                    books.append(known[2])
                    continue
                meta = read_meta(path, single)
                title = meta.title or path.stem
                book = Book(
                    id=short_id(rel),
                    path=path,
                    rel=rel,
                    title=title,
                    authors=tuple(meta.authors),
                    title_sort=meta.title_sort or title,
                    added=meta.added if meta.added is not None else st.st_mtime,
                    size=st.st_size,
                    mtime_ns=st.st_mtime_ns,
                )
                self._meta[rel] = (st.st_size, st.st_mtime_ns, book)
                books.append(book)
            self._books = books
            self._by_id = {b.id: b for b in books}
            self._scanned = time.monotonic()
            return books

    def _files(self):
        """(path, only book in its folder) for every book below root."""
        if not self.root.is_dir():
            return
        for dirpath, dirnames, filenames in os.walk(self.root):
            here = Path(dirpath)
            dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
            if here != self.root and "metadata.db" in filenames:
                dirnames[:] = []
                rel = here.relative_to(self.root).as_posix()
                if rel not in self._skipped:
                    self._skipped.add(rel)
                    self.log(f"skipping {rel}/: a Calibre library inside the library folder")
                continue
            names = sorted(
                n for n in filenames if n.lower().endswith(BOOK_EXTS) and not n.startswith(".")
            )
            for n in names:
                yield here / n, len(names) == 1

    def get(self, book_id: str) -> Book | None:
        self.scan()
        return self._by_id.get(book_id)

    def newest(self) -> list[Book]:
        return sorted(self.scan(), key=lambda b: (-b.added, b.title_sort.casefold()))

    def by_title(self) -> list[Book]:
        return sorted(self.scan(), key=lambda b: (b.title_sort.casefold(), b.rel))

    def authors(self) -> list[Author]:
        books: dict[str, list[Book]] = {}
        names: dict[str, str] = {}
        for b in self.scan():
            for name in b.authors:  # books without one are under Newest and Titles
                key = name.casefold()
                names.setdefault(key, name)
                books.setdefault(key, []).append(b)
        out = [
            Author(
                short_id(key, 10),
                names[key],
                tuple(sorted(bs, key=lambda b: b.title_sort.casefold())),
            )
            for key, bs in books.items()
        ]
        return sorted(out, key=lambda a: author_sort_key(a.name))

    def author(self, author_id: str) -> Author | None:
        return next((a for a in self.authors() if a.id == author_id), None)
