import pytest

from bookprep.cli import toc_entries
from bookprep.extract import Block, Chapter
from bookprep.layout import (
    Gap,
    Hyphenator,
    LayoutOptions,
    Line,
    PageBreak,
    PageGeometry,
    Run,
    break_lines,
    layout_book,
    paginate,
)

TEXT = (
    "Ik ben makelaar in koffi, en woon op de Lauriergracht, No. 37. Het is myn gewoonte "
    "niet, romans te schryven, of zulke dingen, en het heeft dan ook lang geduurd, voor ik "
    "er toe overging een paar riem papier extra te bestellen."
)


def right_edge(runs, measure):
    return runs[-1].x + measure.width(runs[-1].text)


def test_lines_fit_and_keep_all_words(mono):
    lines = break_lines(TEXT, mono, 120, hyphenator=Hyphenator("nl"))
    assert all(right_edge(runs, mono) <= 120 + 1e-6 for runs in lines)
    assert any(runs[-1].text.endswith("-") for runs in lines)  # hyphenation happened
    rebuilt = "".join(r.text for runs in lines for r in runs).replace("-", "")
    assert rebuilt == TEXT.replace(" ", "")  # TEXT has no hyphens of its own


def test_justified_lines_are_flush(mono):
    lines = break_lines(TEXT, mono, 120, justify=True, hyphenator=Hyphenator("nl"))
    for runs in lines[:-1]:
        if len(runs) > 1:
            assert right_edge(runs, mono) == pytest.approx(120, abs=0.02)
    assert right_edge(lines[-1], mono) < 120  # last line stays ragged


def test_first_line_indent(mono):
    lines = break_lines(TEXT, mono, 120, indent=12)
    assert lines[0][0].x == 12
    assert lines[1][0].x == 0


def test_forced_break_and_blank_line(mono):
    lines = break_lines("a b\nc\n\nd", mono, 200, justify=True)
    assert [[r.text for r in runs] for runs in lines] == [["a", "b"], ["c"], [], ["d"]]
    assert lines[0][1].x == 12  # forced-break line is not stretched


def test_center_align(mono):
    (runs,) = break_lines("abcd", mono, 100, align="center")
    assert runs[0].x == pytest.approx((100 - 24) / 2)


def test_hyphenation_dutch_and_english():
    nl = Hyphenator("nl").splits("ziekenhuisopname")
    assert ("ziekenhuis-", "opname") in nl
    en = Hyphenator("en").splits("hyphenation")
    assert ("hyphen-", "ation") in en
    for head, tail in nl + en:
        assert head.endswith("-")
        assert head[:-1] + tail in ("ziekenhuisopname", "hyphenation")


def test_hyphenation_keeps_punctuation_outside_the_word():
    splits = Hyphenator("nl").splits("“bijvoorbeeld,”")
    assert ("“bijvoor-", "beeld,”") in splits


def test_short_words_not_hyphenated():
    assert Hyphenator("nl").splits("huis") == []


def test_breaking_uses_hyphenation(mono):
    lines = break_lines("aaa ziekenhuisopname", mono, 90, hyphenator=Hyphenator("nl"))
    assert [r.text for r in lines[0]] == ["aaa", "ziekenhuis-"]
    assert lines[1][0].text == "opname"


def test_break_after_em_dash_without_hyphenator(mono):
    lines = break_lines("hoed woei\N{EM DASH}Frits", mono, 72)
    assert [r.text for r in lines[0]] == ["hoed", "woei\N{EM DASH}"]
    assert lines[1][0].text == "Frits"


def test_overlong_word_is_split_by_characters(mono):
    lines = break_lines("x" * 25, mono, 60)
    assert [runs[0].text for runs in lines] == ["x" * 10, "x" * 10, "x" * 5]


# --- pagination --------------------------------------------------------------


def para_lines(para, count, chapter=0, keep=False):
    return [Line([Run(0, f"p{para}l{i}")], chapter, para, i, count, keep) for i in range(count)]


def page_texts(pages):
    return [[line.text for _, line in page.lines] for page in pages]


def test_paginate_fills_pages():
    items = [line for p in range(10) for line in para_lines(p, 1)]
    pages = paginate(items, height=60, line_height=10)
    assert [len(p.lines) for p in pages] == [6, 4]
    assert [y for y, _ in pages[0].lines] == [0, 10, 20, 30, 40, 50]


def test_orphan_moves_first_line_to_next_page():
    items = para_lines(0, 2) + para_lines(1, 3)  # para 1 would start on the last row
    pages = paginate(items, height=30, line_height=10)
    assert page_texts(pages) == [["p0l0", "p0l1"], ["p1l0", "p1l1", "p1l2"]]


def test_widow_pulls_previous_line_along():
    items = para_lines(0, 1) + para_lines(1, 4)  # p1l3 would be alone on page 2
    pages = paginate(items, height=40, line_height=10)
    assert page_texts(pages) == [["p0l0", "p1l0", "p1l1"], ["p1l2", "p1l3"]]


def test_widow_and_orphan_together_move_whole_paragraph():
    items = para_lines(0, 1) + para_lines(1, 3)
    pages = paginate(items, height=30, line_height=10)
    assert page_texts(pages) == [["p0l0"], ["p1l0", "p1l1", "p1l2"]]


def test_widow_orphan_can_be_disabled():
    items = para_lines(0, 1) + para_lines(1, 3)
    pages = paginate(items, height=30, line_height=10, widow_orphan=False)
    assert page_texts(pages) == [["p0l0", "p1l0", "p1l1"], ["p1l2"]]


def test_heading_stays_with_following_text():
    items = [*para_lines(0, 2), *para_lines(1, 1, keep=True), Gap(3), *para_lines(2, 1)]
    pages = paginate(items, height=30, line_height=10)
    assert page_texts(pages) == [["p0l0", "p0l1"], ["p1l0", "p2l0"]]
    assert [y for y, _ in pages[1].lines] == [0, 13]  # gap after the heading survives


def test_gap_dropped_at_top_of_page_and_page_break():
    items = [*para_lines(0, 1), PageBreak(), Gap(5), *para_lines(1, 1)]
    pages = paginate(items, height=30, line_height=10)
    assert page_texts(pages) == [["p0l0"], ["p1l0"]]
    assert pages[1].lines[0][0] == 0


def test_layout_book_chapters_start_new_pages_and_toc(mono):
    chapters = [
        Chapter("Een", [Block("heading", "Een"), Block("text", "kort")]),
        Chapter(None, [Block("text", "naamloos")]),
        Chapter("Twee", [Block("heading", "Twee"), Block("break"), Block("text", "ook kort")]),
    ]
    geometry = PageGeometry()
    pages = layout_book(chapters, mono, geometry, LayoutOptions(line_height=16), None)
    assert len(pages) == 3
    assert [p.chapter for p in pages] == [0, 1, 2]
    toc = toc_entries(chapters, pages)
    assert [(e.start_page, e.title) for e in toc] == [(0, "Een"), (2, "Twee")]
    texts = page_texts(pages)
    assert texts[2] == ["Twee", "* * *", "ook kort"]
