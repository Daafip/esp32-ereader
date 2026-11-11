"""The book server's library: which files are books, their metadata and order."""

import os
import shutil
from pathlib import Path

from conftest import make_epub

from bookprep.library import Library, author_sort_key, parse_opf, reader_file_name

OPF = """<?xml version='1.0' encoding='utf-8'?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:opf="http://www.idpf.org/2007/opf">
    <dc:title>{title}</dc:title>
    {creators}
    <dc:contributor opf:role="bkp">calibre (7.6.0)</dc:contributor>
    <meta name="calibre:timestamp" content="{added}"/>
    <meta name="calibre:title_sort" content="{sort}"/>
  </metadata>
</package>
"""


def opf(title, authors, added, sort=None) -> str:
    creators = "".join(f'<dc:creator opf:role="aut">{a}</dc:creator>' for a in authors)
    return OPF.format(title=title, creators=creators, added=added, sort=sort or title)


def test_parse_opf_reads_calibre_metadata():
    meta = parse_opf(opf("Mrs Dalloway", ["Virginia Woolf"], "2026-06-17T12:50:00+00:00").encode())
    assert meta.title == "Mrs Dalloway"
    assert meta.authors == ["Virginia Woolf"]  # the book-producer contributor is not one
    assert meta.added == 1781700600.0


def test_flat_export_with_opf_txt_and_a_nested_calibre_library(tmp_path):
    root = tmp_path / "books"
    root.mkdir()
    make_epub(root / "Het boek - A. Schrijver.epub")
    authors = ["A. Schrijver", "Steven van de Vijver"]
    added = "2026-05-01T10:00:00+00:00"
    (root / "Het boek - A. Schrijver.opf").write_text(opf("Het boek", authors, added, "boek, Het"))
    make_epub(root / "nieuw.epub")  # its own OPF: "Testboek" by "A. Schrijver"
    (root / "nieuw.opf").write_text(opf("Nieuw", ["Zed"], "2026-11-01T10:00:00+00:00"))
    (root / "notes.txt").write_text("Plain text.\n", encoding="utf-8")
    os.utime(root / "notes.txt", (1_700_000_000, 1_700_000_000))  # 2023
    nested = root / "library"  # Calibre's own copies (the DeDRM add-on makes these)
    (nested / "A. Schrijver" / "Het boek (1)").mkdir(parents=True)
    (nested / "metadata.db").write_bytes(b"")
    shutil.copy(root / "nieuw.epub", nested / "A. Schrijver" / "Het boek (1)" / "copy.epub")
    (root / ".hidden").mkdir()
    shutil.copy(root / "nieuw.epub", root / ".hidden" / "x.epub")
    sub = root / "Klassiekers"  # a plain subfolder counts
    sub.mkdir()
    make_epub(sub / "oud.epub")
    os.utime(sub / "oud.epub", (1_600_000_000, 1_600_000_000))

    logged = []
    lib = Library(root, log=logged.append)
    assert [b.rel for b in lib.newest()] == [
        "nieuw.epub", "Het boek - A. Schrijver.epub", "notes.txt", "Klassiekers/oud.epub",
    ]  # fmt: skip
    assert logged == ["skipping library/: a Calibre library inside the library folder"]
    het = next(b for b in lib.scan() if b.title == "Het boek")
    assert het.authors == ("A. Schrijver", "Steven van de Vijver")
    assert het.author == "A. Schrijver & Steven van de Vijver"
    assert het.file_name == "Het_boek_-_A._Schrijver.t5b"
    assert lib.get(het.id) is het
    notes = next(b for b in lib.scan() if b.rel == "notes.txt")
    assert (notes.title, notes.authors, notes.added) == ("notes", (), 1_700_000_000)
    oud = next(b for b in lib.scan() if b.rel == "Klassiekers/oud.epub")
    assert (oud.title, oud.authors) == ("Testboek", ("A. Schrijver",))  # from the EPUB
    # title order uses Calibre's sort title: "boek, Het" before "Klassiekers..."/"notes"
    assert [b.title for b in lib.by_title()] == ["Het boek", "Nieuw", "notes", "Testboek"]
    authors = lib.authors()
    assert [(a.name, len(a.books)) for a in authors] == [
        ("A. Schrijver", 2), ("Steven van de Vijver", 1), ("Zed", 1),
    ]  # fmt: skip
    assert lib.author(authors[0].id) == authors[0]


def test_a_calibre_library_itself_uses_its_metadata_opf(tmp_path):
    root = tmp_path / "Calibre Library"
    folder = root / "Virginia Woolf" / "Mrs Dalloway (1)"
    folder.mkdir(parents=True)
    (root / "metadata.db").write_bytes(b"")
    make_epub(folder / "Mrs Dalloway - Virginia Woolf.epub")
    (folder / "metadata.opf").write_text(
        opf("Mrs Dalloway", ["Virginia Woolf"], "2026-06-17T12:50:00+00:00")
    )
    (books,) = [Library(root).scan()]
    assert [(b.title, b.author) for b in books] == [("Mrs Dalloway", "Virginia Woolf")]


def test_ids_stay_and_a_changed_file_is_read_again(tmp_path):
    make_epub(tmp_path / "a.epub")
    lib = Library(tmp_path, max_age=0)
    (first,) = lib.scan()
    (again,) = lib.scan()
    assert again is first  # metadata read once per file version
    (tmp_path / "a.opf").write_text(opf("Anders", ["B"], "2026-01-01T00:00:00+00:00"))
    os.utime(tmp_path / "a.epub", ns=(first.mtime_ns + 10**9, first.mtime_ns + 10**9))
    (changed,) = lib.scan()
    assert changed.id == first.id and changed.title == "Anders"
    assert changed.version != first.version


def test_missing_folder_is_an_empty_library(tmp_path):
    assert Library(tmp_path / "nope").scan() == []


def test_reader_file_names_match_the_web_page():
    # index.html fileNameFor(): slug of the stem cut to 64 - 5 bytes, then ".t5b"
    assert reader_file_name(Path("Ga lekker zélf - Japke-d. Bouma.epub")) == (
        "Ga_lekker_z_lf_-_Japke-d._Bouma.t5b"
    )
    long = reader_file_name(Path("x" * 200 + ".epub"))
    assert long == "x" * 59 + ".t5b"


def test_authors_sort_by_surname():
    names = ["Steven van de Vijver", "Woolf, Virginia", "Tessa Afshar", "Unknown"]
    assert sorted(names, key=author_sort_key) == [
        "Tessa Afshar", "Unknown", "Steven van de Vijver", "Woolf, Virginia",
    ]  # fmt: skip
