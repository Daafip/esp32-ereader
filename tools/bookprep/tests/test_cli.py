from bookprep.cli import main, slug
from bookprep.t5b import T5BFile


def test_end_to_end_epub(sample_epub, tmp_path, font_path):
    out = tmp_path / "out"
    rc = main([str(sample_epub), "--out", str(out), "--preview", "--preview-pages", "2"])
    assert rc == 0
    book = T5BFile(out / "testboek.t5b")
    assert book.header.title == "Testboek"
    assert book.header.author == "A. Schrijver"
    assert (book.header.page_w, book.header.page_h, book.header.stride) == (250, 122, 32)
    assert [e.title for e in book.toc] == ["Hoofdstuk 1", "Hoofdstuk 2"]
    assert book.toc[0].start_page == 0
    assert book.toc[1].start_page > 0  # chapter 2 starts on its own page
    report = (out / "testboek.report.txt").read_text(encoding="utf-8")
    assert "language: nl (hyphenation: nl)" in report
    assert "all characters present in the font" in report
    assert len(list((out / "testboek.preview").glob("page-*.png"))) == 2
    assert (out / "testboek.contact.png").exists()


def test_end_to_end_txt_and_overrides(tmp_path, font_path):
    src = tmp_path / "Mijn boek!.txt"
    src.write_text("HOOFDSTUK I\n\n" + "Tekst. " * 300, encoding="utf-8")
    rc = main([str(src), "--out", str(tmp_path), "--title", "Titel", "--lang", "en", "--mono"])
    assert rc == 0
    book = T5BFile(tmp_path / "Mijn_boek.t5b")
    assert book.header.title == "Titel"
    assert book.header.page_count > 1
    assert [e.title for e in book.toc] == ["HOOFDSTUK I"]


def test_missing_input(tmp_path, font_path, capsys):
    assert main([str(tmp_path / "nope.epub")]) == 2
    assert "not found" in capsys.readouterr().err


def test_slug():
    assert slug("Max Havelaar (1860)") == "Max_Havelaar_1860"
    assert slug("***") == "book"
