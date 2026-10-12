from bookprep.extract import extract, extract_txt, html_to_blocks


def kinds(chapter):
    return [b.kind for b in chapter.blocks]


def test_epub_metadata_and_chapters(sample_epub):
    book = extract(sample_epub)
    assert book.title == "Testboek"
    assert book.author == "A. Schrijver"
    assert book.language == "nl"
    # cover (image only) is skipped; the TOC fragment #c2 starts chapter 2;
    # ch1b.xhtml is not in the TOC and continues chapter 2
    assert [c.title for c in book.chapters] == ["Hoofdstuk 1", "Hoofdstuk 2"]
    one, two = book.chapters
    assert kinds(one) == ["heading", "text", "text", "break", "text", "image"]
    assert one.blocks[0].text == "Hoofdstuk 1"  # footnote marker stripped
    assert one.blocks[1].text == "Eerste alinea met witruimte."
    assert one.blocks[2].text == "Regel een\nregel twee"
    assert one.blocks[5].text == "[image]"
    assert [b.text for b in two.blocks] == [
        "Hoofdstuk 2",
        "Tweede hoofdstuk begint hier.",
        "Vervolg van hoofdstuk twee.",
    ]


def test_html_nested_blocks_and_inline_markup():
    html = (
        "<html><head><title>x</title><style>p{}</style></head><body>"
        "<div><p>Een <em>schuin</em> woord.</p><blockquote><p>Citaat</p></blockquote></div>"
        "<p>   </p><p>\N{NO-BREAK SPACE}</p><!-- comment -->"
        "<ul><li>een</li><li>twee</li></ul>"
        "</body></html>"
    )
    blocks = html_to_blocks(html)
    assert [(b.kind, b.text) for b in blocks] == [
        ("text", "Een schuin woord."),
        ("text", "Citaat"),
        ("text", "een"),
        ("text", "twee"),
    ]


def test_anchor_on_old_style_a_name():
    html = "<body><p>voor</p><a name='h2'></a><p>na</p></body>"
    blocks = html_to_blocks(html, {"h2": "Twee"})
    assert [(b.kind, b.text) for b in blocks] == [
        ("text", "voor"),
        ("anchor", "Twee"),
        ("text", "na"),
    ]


def test_txt_gutenberg_and_chapters(tmp_path):
    path = tmp_path / "boek.txt"
    path.write_text(
        "Header junk\n*** START OF THE PROJECT GUTENBERG EBOOK X ***\n\n"
        "Voorwoord dat\nhard is afgebroken.\n\n"
        "HOOFDSTUK I\n\nEerste alinea.\n\nTweede\nalinea.\n\n"
        "II.\n\nNog een.\n\ncivil\n\n"
        "*** END OF THE PROJECT GUTENBERG EBOOK X ***\nLicence junk\n",
        encoding="utf-8",
    )
    book = extract_txt(path)
    assert book.title == "boek"
    assert [c.title for c in book.chapters] == [None, "HOOFDSTUK I", "II."]
    assert book.chapters[0].blocks[0].text == "Voorwoord dat hard is afgebroken."
    assert [b.text for b in book.chapters[1].blocks] == [
        "HOOFDSTUK I",
        "Eerste alinea.",
        "Tweede alinea.",
    ]
    assert book.chapters[2].blocks[-1].text == "civil"  # lower case: not a roman numeral
    assert "junk" not in str(book)


def test_txt_cp1252_fallback(tmp_path):
    path = tmp_path / "oud.txt"
    path.write_bytes("Café".encode("cp1252"))
    assert extract_txt(path).chapters[0].blocks[0].text == "Café"
