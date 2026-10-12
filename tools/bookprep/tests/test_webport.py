"""Parity between bookprep (Python) and its browser port firmware/web/bookprep.js.

Runs the same inputs through both and compares the results; needs Node.js."""

import base64
import json
import shutil
import subprocess
import zipfile
from pathlib import Path

import pytest

from bookprep.cli import slug, toc_entries
from bookprep.devpages import SAMPLE_TEXT
from bookprep.extract import Block, Chapter, extract_txt
from bookprep.layout import (
    SCREENS,
    Hyphenator,
    LayoutOptions,
    PageGeometry,
    break_lines,
    layout_book,
    line_height_for,
)
from bookprep.normalize import normalize_text
from bookprep.t5b import TocEntry, encode_str, write_t5b
from bookprep.webassets import export_hyphenation, hyphenation_index

ROOT = Path(__file__).resolve().parents[3]
CHECK = ROOT / "firmware" / "web" / "test" / "port_check.js"
pytestmark = pytest.mark.skipif(shutil.which("node") is None, reason="needs node")

ENGLISH = (
    "It was the best of times, it was the worst of times, it was the age of wisdom, it was "
    "the age of foolishness, it was the epoch of belief, it was the epoch of incredulity, it "
    "was the season of Light, it was the season of Darkness, it was the spring of hope."
)
NORMALIZE = [
    '"Hallo," zei hij.', "Ze zei 'ja' en ging.", "auto's en foto's", "'s Avonds om 't huis",
    "in de jaren '90", "('t is laat)", "Wacht...", "Wacht. . . nu", "ja--nee",
    'regel een\n"regel twee"', "zie\N{SOFT HYPHEN}ken\N{ZERO WIDTH SPACE}huis\N{THIN SPACE}10"
    "\N{NARROW NO-BREAK SPACE}km", "Cafe\N{COMBINING ACUTE ACCENT}", "'sé 'tje 'n'",
    *SAMPLE_TEXT, ENGLISH,
]  # fmt: skip


def words(text):
    return [w for w in normalize_text(text).replace("\n", " ").split(" ") if w]


def run_node(cases, tmp_path):
    path = tmp_path / "cases.json"
    path.write_text(json.dumps(cases), encoding="utf-8")
    result = subprocess.run(
        ["node", str(CHECK), str(path)], capture_output=True, text=True, check=True, timeout=120
    )
    return json.loads(result.stdout)


def as_runs(lines):
    return [[(round(r.x, 2), r.text) for r in runs] for runs in lines]


def assert_runs_equal(js, py):
    assert len(js) == len(py)
    for jl, pl in zip(js, py, strict=True):
        assert [r["text"] for r in jl] == [t for _, t in pl]
        for jr, (px, _) in zip(jl, pl, strict=True):
            assert jr["x"] == pytest.approx(px, abs=0.011)


def test_port_matches_python(tmp_path, sample_epub, mono):
    langs = ["nl_NL", "en_GB"]
    nl, en = Hyphenator("nl_NL"), Hyphenator("en_GB")
    split_words = [("nl_NL", w) for t in SAMPLE_TEXT for w in words(t)]
    split_words += [("en_GB", w) for w in words(ENGLISH)]
    split_words += [
        ("nl_NL", "\N{LEFT DOUBLE QUOTATION MARK}ziekenhuisopname,\N{RIGHT DOUBLE QUOTATION MARK}")
    ]

    line_cases = []
    for text, lang in [(SAMPLE_TEXT[0], "nl_NL"), (SAMPLE_TEXT[1], "nl_NL"), (ENGLISH, "en_GB")]:
        text = normalize_text(text)
        for width in (90, 120, 242):
            for justify in (False, True):
                line_cases.append({"text": text, "width": width, "indent": 13, "justify": justify,
                                   "align": "left", "lang": lang})  # fmt: skip
    line_cases.append({"text": "Hoofdstuk \N{EM DASH} een\nkort", "width": 60, "indent": 0,
                       "justify": True, "align": "center", "lang": None})  # fmt: skip

    chapters = [
        Chapter("Een", [Block("heading", "Eerste Hoofdstuk"), *[Block("text", normalize_text(t)) for t in SAMPLE_TEXT]]),
        Chapter(None, [Block("text", "naamloos"), Block("break"), Block("image", "[image]")]),
        Chapter("Twee", [Block("heading", "Twee"), Block("text", normalize_text(ENGLISH * 3))]),
    ]  # fmt: skip
    layout_cases = []
    big = SCREENS["t5-47"]
    for screen, margin, lh, spacing in [(None, 4, 16, 0), (None, 8, 13, 3), (big, 24, 34, 0)]:
        if screen:  # the JS side derives the page from its own screens table
            geom = screen.geometry(margin=margin)
            js_geom = {"screen": screen.id, "margin": margin}
        else:
            geom = PageGeometry(margin_left=margin, margin_right=margin, margin_top=margin)
            js_geom = {"marginLeft": margin, "marginRight": margin, "marginTop": margin}
        opts = LayoutOptions(line_height=lh, indent=13, paragraph_spacing=spacing)
        pages = layout_book(chapters, mono, geom, opts, nl)
        layout_cases.append({
            "chapters": [{"title": c.title, "blocks": [{"kind": b.kind, "text": b.text} for b in c.blocks]} for c in chapters],
            "geometry": js_geom,
            "options": {"lineHeight": lh, "indent": 13, "paragraphSpacing": spacing, "justify": True,
                        "chapterPageBreak": True, "widowOrphan": True},
            "lang": "nl_NL",
            "expected": {
                "pages": [[[y, line.text] for y, line in p.lines] for p in pages],
                "toc": [[e.start_page, e.title] for e in toc_entries(chapters, pages)],
            },
        })  # fmt: skip

    txt = (
        b"*** START OF THE PROJECT GUTENBERG EBOOK X ***\n\nVoorwoord\nhard.\n\n"
        b"HOOFDSTUK I\n\nEerste.\n\nII.\n\nNog een.\n\ncivil\n\n*** END OF THE PROJECT GUTENBERG EBOOK X ***"
    )  # fmt: skip
    txt_path = tmp_path / "boek.txt"
    txt_path.write_bytes(txt)
    cp1252 = tmp_path / "oud.txt"
    cp1252.write_bytes("Caf\N{LATIN SMALL LETTER E WITH ACUTE}".encode("cp1252"))

    pages = [bytes([i]) * 3904 for i in range(3)]
    toc = [TocEntry(0, "Begin"), TocEntry(2, "H\N{LATIN SMALL LETTER E WITH DIAERESIS}t einde")]
    t5b_path = tmp_path / "x.t5b"
    write_t5b(t5b_path, title="Max \N{EM DASH} " + "\N{LATIN SMALL LETTER E WITH ACUTE}" * 40,
              author="M", page_w=250, page_h=122, toc=toc, pages=pages)  # fmt: skip

    encode_cases = [
        ["\N{LATIN SMALL LETTER E WITH ACUTE}" * 40, 64],
        ["abc", 4],
        ["\N{EM DASH}x", 3],
    ]
    slugs = ["Max Havelaar (1860)", "***", "Mijn boek!"]
    aliases = hyphenation_index()["aliases"]
    languages = ["nl", "nl-NL", "en", "en-US", "en-AU", "EN_gb", "de", "de-CH", "fr-BE", "it", ""]

    cases = {
        "normalize": NORMALIZE,
        "patterns": {lang: export_hyphenation(lang) for lang in langs},
        "splits": split_words,
        "aliases": aliases,
        "languages": languages,
        "lines": line_cases,
        "layout": layout_cases,
        "txt": [
            ["boek.txt", base64.b64encode(txt).decode()],
            ["oud.txt", base64.b64encode(cp1252.read_bytes()).decode()],
        ],
        "t5b": {
            "title": "Max \N{EM DASH} " + "\N{LATIN SMALL LETTER E WITH ACUTE}" * 40,
            "author": "M",
            "pageW": 250,
            "pageH": 122,
            "toc": [{"startPage": e.start_page, "title": e.title} for e in toc],
            "pages": [base64.b64encode(p).decode() for p in pages],
        },
        "encode": encode_cases,
        "slug": slugs,
        "lineHeights": [[size, f] for size in range(9, 23) for f in (1.0, 1.1, 1.2, 1.3, 1.45)],
        "zip": {"path": str(sample_epub)},
    }
    js = run_node(cases, tmp_path)

    assert js["normalize"] == [normalize_text(t) for t in NORMALIZE]
    py_splits = [(nl if lang == "nl_NL" else en).splits(w) for lang, w in split_words]
    assert js["splits"] == [[list(s) for s in splits] for splits in py_splits]
    assert sum(map(len, py_splits)) > 50  # the comparison covered real hyphenation
    # pyphen's truncation fallback, limited to the dictionaries the browser bundles
    # (German is not bundled, so no hyphenation for it in the browser).
    assert js["languages"] == [
        "nl_NL", "nl_NL", "en_GB", "en_US", "en_GB", "en_GB", None, None, "fr", None, None,
    ]  # fmt: skip

    for case, got in zip(line_cases, js["lines"], strict=True):
        hyph = {"nl_NL": nl, "en_GB": en}.get(case["lang"])
        want = break_lines(case["text"], mono, case["width"], indent=case["indent"], hyphenator=hyph,
                           justify=case["justify"], align=case["align"])  # fmt: skip
        assert_runs_equal(got, as_runs(want))

    for case, got in zip(layout_cases, js["layout"], strict=True):
        assert got == case["expected"]
    assert len(layout_cases[2]["expected"]["pages"]) < len(layout_cases[0]["expected"]["pages"])

    # one table of readers: Python's SCREENS, bookprep.js's screens
    assert js["screens"] == {
        sc.id: {"id": sc.id, "name": sc.name, "width": sc.width, "height": sc.height,
                "statusBar": sc.status_bar, "size": sc.font_size, "margin": sc.margin,
                "marginBottom": sc.margin_bottom}
        for sc in SCREENS.values()
    }  # fmt: skip

    for got, path in zip(js["txt"], [txt_path, cp1252], strict=True):
        want = extract_txt(path)
        assert got["title"] == want.title
        assert [c["title"] for c in got["chapters"]] == [c.title for c in want.chapters]
        assert [[(b["kind"], b["text"]) for b in c["blocks"]] for c in got["chapters"]] == [
            [(b.kind, b.text) for b in c.blocks] for c in want.chapters
        ]

    assert base64.b64decode(js["t5b"]) == t5b_path.read_bytes()
    assert [base64.b64decode(e) for e in js["encode"]] == [
        encode_str(t, n) for t, n in encode_cases
    ]
    assert js["slug"] == [slug(s) for s in slugs]
    assert js["lineHeights"] == [line_height_for(s, f) for s, f in cases["lineHeights"]]
    assert line_height_for(10) == 12 and line_height_for(13) == 16
    with zipfile.ZipFile(sample_epub) as z:
        assert js["zip"]["names"] == sorted(z.namelist())
        assert js["zip"]["container"] == z.read("META-INF/container.xml").decode()
