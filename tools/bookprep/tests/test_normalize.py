import string

import pytest

from bookprep.normalize import NBSP, GlyphFallback, font_charset, normalize_text


@pytest.mark.parametrize(
    ("raw", "expected"),
    [
        ('"Hallo," zei hij.', "“Hallo,” zei hij."),
        ("Ze zei 'ja' en ging.", "Ze zei ‘ja’ en ging."),
        ("auto's en foto's", "auto’s en foto’s"),
        ("'s Avonds om 't huis", "’s Avonds om ’t huis"),
        ("in de jaren '90", "in de jaren ’90"),
        ("('t is laat)", "(’t is laat)"),
        ("Wacht...", "Wacht\N{HORIZONTAL ELLIPSIS}"),
        ("Wacht. . . nu", "Wacht\N{HORIZONTAL ELLIPSIS} nu"),
        ("ja--nee", "ja\N{EM DASH}nee"),
        ('regel een\n"regel twee"', "regel een\n“regel twee”"),
    ],
)
def test_normalize_text(raw, expected):
    assert normalize_text(raw) == expected


def test_invisible_characters():
    raw = (
        "zie\N{SOFT HYPHEN}ken\N{ZERO WIDTH SPACE}huis"
        "\N{THIN SPACE}10\N{NARROW NO-BREAK SPACE}km"
        "\N{ZERO WIDTH NO-BREAK SPACE}"
    )
    assert normalize_text(raw) == f"ziekenhuis 10{NBSP}km"


def test_nfc():
    assert normalize_text("Cafe\N{COMBINING ACUTE ACCENT}") == "Café"


def test_glyph_fallback_maps_and_reports():
    ascii_font = GlyphFallback.for_charset(set(string.printable))
    out = ascii_font(f"“Café” — ﬁne{NBSP}dag 漢")
    assert out == f'"Cafe" - fine{NBSP}dag ?'
    assert ascii_font.replaced[("é", "e")] == 1
    assert ascii_font.replaced[("“", '"')] == 1
    assert ascii_font.missing["漢"] == 1
    report = "\n".join(ascii_font.report())
    assert "MISSING  U+6F22" in report


def test_glyph_fallback_keeps_present_characters():
    fix = GlyphFallback.for_charset(set("abc“”"))
    assert fix("“abc”\n") == "“abc”\n"
    assert not fix.replaced
    assert not fix.missing


def test_font_charset(font_path):
    charset = font_charset(font_path)
    assert {"a", "é", "ë", "’", "“", "\N{HORIZONTAL ELLIPSIS}", "\N{EM DASH}"} <= charset
