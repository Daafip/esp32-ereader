"""Text normalisation and font-coverage fallbacks.

Two steps:

1. :func:`normalize_text` is font independent: NFC, typographic quotes, ellipsis,
   dashes, removal of soft hyphens and zero-width characters, odd spaces.
2. :class:`GlyphFallback` maps characters the chosen font lacks to replacements it
   has (curly quote -> straight quote, accented letter -> base letter, ...) and
   counts what it did, for the report.

Non-ASCII characters are written as escapes on purpose: several are invisible.
"""

from __future__ import annotations

import re
import unicodedata
from collections import Counter
from collections.abc import Callable
from pathlib import Path

NBSP = "\u00a0"

# soft hyphen, zero-width space/non-joiner/joiner, word joiner, BOM
_REMOVE = dict.fromkeys(map(ord, "\u00ad\u200b\u200c\u200d\u2060\ufeff"))
_SPACES = {ord(c): " " for c in "\t\u2002\u2003\u2004\u2005\u2006\u2008\u2009\u200a\u3000"}
_NB_SPACES = {ord(c): NBSP for c in "\u2007\u202f"}  # figure space, narrow NBSP

# Dutch elisions like 's avonds, 't huis, 'n beetje, 'k ben, and years like '90.
_ELISION = re.compile(r"(?<![\w\u2019])'(?=(?:s|t|n|k|em|r)\b|\d\d\b)", re.I)
_OPEN_CONTEXT = r"(?:^|(?<=[\s(\[{\u2014\u2013\-/]))"

FALLBACKS: dict[str, str] = {
    "\u2018": "'", "\u2019": "'", "\u201a": ",", "\u201b": "'",
    "\u201c": '"', "\u201d": '"', "\u201e": '"', "\u201f": '"',
    "\u00ab": '"', "\u00bb": '"', "\u2039": "'", "\u203a": "'",
    "\u2032": "'", "\u2033": '"',
    "\u2010": "-", "\u2011": "-", "\u2012": "-", "\u2013": "-", "\u2014": "-",
    "\u2015": "-", "\u2212": "-",
    "\u2026": "...", "\u2022": "*", "\u00b7": ".", "\u2044": "/",
    "\ufb00": "ff", "\ufb01": "fi", "\ufb02": "fl", "\ufb03": "ffi", "\ufb04": "ffl",
    "\u00bc": "1/4", "\u00bd": "1/2", "\u00be": "3/4",
    "\u2122": "TM", "\u00a9": "(c)", "\u00ae": "(R)",
    "\u00c6": "AE", "\u00e6": "ae", "\u0152": "OE", "\u0153": "oe", "\u00df": "ss",
}  # fmt: skip
MISSING = "?"


def normalize_text(text: str) -> str:
    text = unicodedata.normalize("NFC", text)
    text = text.translate(_REMOVE).translate(_SPACES).translate(_NB_SPACES)
    text = re.sub(r"\.\s?\.\s?\.", "\u2026", text)
    text = re.sub(r"(?<!-)--(?!-)", "\u2014", text)
    return smart_quotes(text)


def smart_quotes(text: str) -> str:
    text = _ELISION.sub("\u2019", text)
    text = re.sub(_OPEN_CONTEXT + '"', "\u201c", text, flags=re.M)
    text = text.replace('"', "\u201d")
    text = re.sub(_OPEN_CONTEXT + "'", "\u2018", text, flags=re.M)
    return text.replace("'", "\u2019")


def font_charset(font_path: Path) -> set[str]:
    """Characters the font has glyphs for (from its cmap)."""
    from fontTools.ttLib import TTFont

    with TTFont(str(font_path), fontNumber=0, lazy=True) as font:
        cmap = font.getBestCmap() or {}
    return {chr(cp) for cp in cmap}


class GlyphFallback:
    """Replace characters missing from the font and count the replacements."""

    def __init__(self, has_glyph: Callable[[str], bool]):
        self.has_glyph = has_glyph
        self.replaced: Counter[tuple[str, str]] = Counter()
        self.missing: Counter[str] = Counter()
        self._cache: dict[str, str] = {}

    @classmethod
    def for_charset(cls, charset: set[str]) -> GlyphFallback:
        return cls(charset.__contains__)

    def _resolve(self, ch: str) -> str:
        if ch in ("\n", NBSP) or self.has_glyph(ch):  # NBSP is rendered as a space
            return ch
        candidates = []
        if ch in FALLBACKS:
            candidates.append(FALLBACKS[ch])
        base = "".join(c for c in unicodedata.normalize("NFD", ch) if not unicodedata.combining(c))
        if base and base != ch:
            candidates.append(base)
        for candidate in candidates:
            if all(self.has_glyph(c) for c in candidate):
                return candidate
        return ""  # signals "missing"

    def __call__(self, text: str) -> str:
        out = []
        for ch in text:
            if ch not in self._cache:
                self._cache[ch] = self._resolve(ch)
            repl = self._cache[ch]
            if repl == ch:
                out.append(ch)
            elif repl:
                self.replaced[(ch, repl)] += 1
                out.append(repl)
            else:
                self.missing[ch] += 1
                out.append(MISSING)
        return "".join(out)

    def report(self) -> list[str]:
        lines = []
        for (ch, repl), n in self.replaced.most_common():
            lines.append(f"  replaced U+{ord(ch):04X} {ch!r} -> {repl!r}  x{n}")
        for ch, n in self.missing.most_common():
            name = unicodedata.name(ch, "?")
            lines.append(f"  MISSING  U+{ord(ch):04X} {ch!r} {name} -> {MISSING!r}  x{n}")
        return lines
