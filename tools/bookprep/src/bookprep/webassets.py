"""Assets for the reader's web page (Wi-Fi transfer + in-browser converter).

Builds everything the browser converter needs and embeds it in the firmware:

- hyphenation patterns exported from pyphen's own parser, so the browser port
  (firmware/web/bookprep.js) hyphenates exactly like this package
- reading fonts as Latin subsets in WOFF2: Ubuntu (from the pixi env) and three
  SIL OFL fonts downloaded from a pinned google/fonts commit (checked by SHA-256);
  OpenDyslexic as its author's own WOFF2, unmodified (its licence reserves the
  name, so no subsetting)
- the page itself (firmware/web/index.html, bookprep.js)

Run: ``pixi run web``. Writes the files to --out (for tests and local serving) and
a C header with all of them (gzipped where that helps) to --header.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import sys
import urllib.request
from dataclasses import dataclass
from pathlib import Path

import pyphen

WEB_DIR = Path(__file__).resolve().parents[4] / "firmware" / "web"

# German (de_*) is left out on purpose: its patterns alone are 310 KB gzipped.
HYPHENATION = ["nl_NL", "en_GB", "en_US", "fr"]

OFL_LITERATA = "8742963604cd89dc81437811a850018fc03b2bfad686d7422c8235967c87614e"
OFL_ATKINSON = "f32d22b3908fcad2c86a74000614ec22e6a7f66ea7e867e616026a27aebdc143"
OFL_MERRIWEATHER = "715bd349bfa6116f99e351e1548321dcfa25b91df65502d22ca8d624557697d2"
GOOGLE_FONTS = (
    "https://raw.githubusercontent.com/google/fonts/bd8f81ddb5c74d5c8897b36ad88b440266245103"
)
OPENDYSLEXIC = (
    "https://raw.githubusercontent.com/antijingoist/opendyslexic/"
    "1824da5c0e41dc3e13ffc7f3a636dcaf695d61b7"
)

# Codepoints kept in the font subsets: Latin-1, Windows-1252 extras, typography.
SUBSET = [
    *range(0x20, 0x7F), *range(0xA0, 0x100),
    0x152, 0x153, 0x160, 0x161, 0x178, 0x17D, 0x17E,
    0x2013, 0x2014, 0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E,
    0x2020, 0x2021, 0x2022, 0x2026, 0x2030, 0x2039, 0x203A, 0x20AC, 0x2122,
]  # fmt: skip


@dataclass(frozen=True)
class FontSpec:
    id: str
    name: str
    licence: str
    source: str  # "env:<file in $PREFIX/fonts>", a google/fonts path or an https URL
    sha256: str = ""
    axes: tuple[tuple[str, float], ...] = ()
    licence_file: tuple[str, str] = ("", "")  # google/fonts path or URL, sha256
    as_is: bool = False  # serve the downloaded WOFF2 unchanged (no subset)


FONTS = [
    FontSpec("ubuntu", "Ubuntu", "Ubuntu Font Licence 1.0", "env:Ubuntu-R.ttf"),
    FontSpec(
        "literata", "Literata", "SIL OFL 1.1", "ofl/literata/Literata%5Bopsz,wght%5D.ttf",
        "b41138c9373112f32abb589cc22e8674b06ed4048b0c513be922bdd26f274440",
        (("opsz", 12), ("wght", 400)),
        ("ofl/literata/OFL.txt", OFL_LITERATA),
    ),
    FontSpec(
        "atkinson", "Atkinson Hyperlegible", "SIL OFL 1.1",
        "ofl/atkinsonhyperlegible/AtkinsonHyperlegible-Regular.ttf",
        "7fb917c89019896d0b52ee84b7cbb3304c18cb90b19a62f5e32712bd23e97669",
        (),
        ("ofl/atkinsonhyperlegible/OFL.txt", OFL_ATKINSON),
    ),
    FontSpec(
        "merriweather", "Merriweather", "SIL OFL 1.1",
        "ofl/merriweather/Merriweather%5Bopsz,wdth,wght%5D.ttf",
        "d0ed0e359e396af7ad05e73dffd11a3a4c326ea0d0283c56bd9361cb2cc86a96",
        (("opsz", 18), ("wdth", 100), ("wght", 400)),
        ("ofl/merriweather/OFL.txt", OFL_MERRIWEATHER),
    ),
    FontSpec(
        "opendyslexic", "OpenDyslexic", "SIL OFL 1.1, Reserved Font Name (unmodified)",
        f"{OPENDYSLEXIC}/compiled/OpenDyslexic-Regular.woff2",
        "0441bc21071e42db57c217f93fbc48d3b55a2987c02814c94dc93621c42e8695",
        (),
        (f"{OPENDYSLEXIC}/OFL.txt",
         "caafcccfb70fc72458fcbda812ec8f0a06cb300cbabb87eabbb30b946124394b"),
        as_is=True,
    ),
]  # fmt: skip


# --- hyphenation ------------------------------------------------------------------


def export_hyphenation(name: str) -> str:
    """pyphen's parsed patterns as text: 'maxlen N', then 'key start values' lines."""
    hd = pyphen.HyphDict(pyphen.LANGUAGES[name])
    lines = [f"maxlen {hd.maxlen}"]
    for key, (start, values) in sorted(hd.patterns.items()):
        if any(getattr(v, "data", None) for v in values):
            raise ValueError(f"{name}: nonstandard pattern {key!r} is not supported by the port")
        lines.append(f"{key} {start} {''.join(str(int(v)) for v in values)}")
    return "\n".join(lines) + "\n"


def hyphenation_index() -> dict:
    """Language codes (pyphen's table, lower-cased) that resolve to a bundled dictionary."""
    bundled = {pyphen.LANGUAGES[n].name: n for n in HYPHENATION}
    aliases = {
        code.lower(): bundled[path.name]
        for code, path in pyphen.LANGUAGES.items()
        if path.name in bundled
    }
    return {"dictionaries": HYPHENATION, "aliases": dict(sorted(aliases.items()))}


# --- fonts --------------------------------------------------------------------------


def pinned_download(source: str, sha256: str, cache: Path) -> Path:
    """A file at a pinned commit (google/fonts path or full URL), cached and checked
    by SHA-256."""
    url = source if source.startswith("https://") else f"{GOOGLE_FONTS}/{source}"
    name = source.split("/", 4)[-1] if source.startswith("https://") else source
    path = cache / name.replace("/", "_").replace("%5B", "[").replace("%5D", "]")
    if not path.exists():
        cache.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(url, timeout=60) as r:
            path.write_bytes(r.read())
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != sha256:
        raise ValueError(f"{path.name}: SHA-256 {digest} does not match the pinned {sha256}")
    return path


def font_source(spec: FontSpec, cache: Path) -> Path:
    if spec.source.startswith("env:"):
        for base in (Path(sys.prefix) / "fonts", Path(sys.prefix) / "Library" / "fonts"):
            if (base / spec.source[4:]).exists():
                return base / spec.source[4:]
        raise FileNotFoundError(spec.source)
    return pinned_download(spec.source, spec.sha256, cache)


def licences(cache: Path, fonts: bool = True) -> str:
    """Notices for everything the page ships: fonts (copyright and licence from
    each font's name table, plus the full OFL text) and hyphenation patterns."""
    from fontTools.ttLib import TTFont

    out = ["Licences of the fonts and hyphenation patterns served by the T5 reader.", ""]
    for spec in FONTS if fonts else []:
        names = TTFont(str(font_source(spec, cache)))["name"]
        out += [f"== Font: {spec.name} ({spec.licence})", ""]
        for name_id in (0, 13, 14):
            text = names.getDebugName(name_id)
            if text:
                out += [text, ""]
        if spec.licence_file[0]:
            out += [pinned_download(*spec.licence_file, cache).read_text(encoding="utf-8"), ""]
    readmes = Path(pyphen.__file__).parent / "dictionaries"
    for name in HYPHENATION:
        out += [f"== Hyphenation patterns: {name} (from pyphen {pyphen.__version__})", ""]
        readme = readmes / f"README_hyph_{name}.txt"
        if readme.exists():
            out += [readme.read_text(encoding="utf-8", errors="replace"), ""]
        else:
            out += [
                "OpenTaal patterns as distributed with pyphen; see "
                "https://github.com/Kozea/Pyphen/tree/main/pyphen/dictionaries",
                "",
            ]
    return "\n".join(out)


def build_font(spec: FontSpec, cache: Path) -> bytes:
    from fontTools import subset
    from fontTools.ttLib import TTFont
    from fontTools.varLib.instancer import instantiateVariableFont

    if spec.as_is:
        return font_source(spec, cache).read_bytes()
    font = TTFont(str(font_source(spec, cache)))
    if spec.axes:
        font = instantiateVariableFont(font, dict(spec.axes))
    options = subset.Options()
    options.flavor = "woff2"
    options.layout_features = ["kern", "liga"]
    options.name_IDs = ["*"]
    options.notdef_outline = True
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=SUBSET)
    subsetter.subset(font)
    font.flavor = "woff2"
    out = io.BytesIO()
    font.save(out)
    return out.getvalue()


# --- assembly ---------------------------------------------------------------------


@dataclass(frozen=True)
class Asset:
    path: str  # URL path
    mime: str
    data: bytes
    gzip: bool


def build_assets(cache: Path, web_dir: Path = WEB_DIR, fonts: bool = True) -> list[Asset]:
    assets = [
        Asset("/", "text/html; charset=utf-8", (web_dir / "index.html").read_bytes(), True),
        Asset("/bookprep.js", "text/javascript", (web_dir / "bookprep.js").read_bytes(), True),
        Asset("/hyph/index.json", "application/json",
              json.dumps(hyphenation_index()).encode(), True),
        Asset("/licenses.txt", "text/plain; charset=utf-8", licences(cache, fonts).encode(), True),
    ]  # fmt: skip
    for name in HYPHENATION:
        assets.append(
            Asset(f"/hyph/{name}.txt", "text/plain", export_hyphenation(name).encode(), True)
        )
    if fonts:
        index = [{"id": f.id, "name": f.name, "url": f"/fonts/{f.id}.woff2"} for f in FONTS]
        assets.append(
            Asset("/fonts/index.json", "application/json", json.dumps(index).encode(), True)
        )
        for spec in FONTS:
            assets.append(
                Asset(f"/fonts/{spec.id}.woff2", "font/woff2", build_font(spec, cache), False)
            )
    return assets


def write_files(assets: list[Asset], out: Path) -> None:
    for a in assets:
        path = out / ("index.html" if a.path == "/" else a.path.lstrip("/"))
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(a.data)


def write_header(assets: list[Asset], header: Path) -> int:
    parts = [
        "// Generated by `pixi run web` (tools/bookprep/src/bookprep/webassets.py). Do not edit.\n"
        "// The reader's web page, the in-browser converter and its fonts/hyphenation data.\n"
        "#pragma once\n\n#include <stddef.h>\n#include <stdint.h>\n\n"
        "struct WebAsset {\n  const char *path;\n  const char *mime;\n  const uint8_t *data;\n"
        "  size_t len;\n  bool gzip;  // send with Content-Encoding: gzip\n};\n\n"
    ]
    total = 0
    table = []
    for i, a in enumerate(assets):
        data = gzip.compress(a.data, compresslevel=9, mtime=0) if a.gzip else a.data
        total += len(data)
        rows = ",".join(("\n  " if j % 24 == 0 else "") + f"0x{b:02x}" for j, b in enumerate(data))
        parts.append(f"// {a.path}: {len(a.data)} bytes{', gzipped' if a.gzip else ''}\n")
        parts.append(f"static const uint8_t kWebAsset{i}[{len(data)}] = {{{rows}\n}};\n\n")
        gz = "true" if a.gzip else "false"
        table.append(f'  {{"{a.path}", "{a.mime}", kWebAsset{i}, {len(data)}, {gz}}},')
    parts.append("static const WebAsset kWebAssets[] = {\n" + "\n".join(table) + "\n};\n")
    parts.append(f"constexpr size_t kWebAssetCount = {len(assets)};  // {total} bytes in flash\n")
    header.write_text("".join(parts), encoding="ascii")
    return total


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="webassets", description=__doc__.splitlines()[0])
    p.add_argument("--header", type=Path, help="C header to write")
    p.add_argument("--out", type=Path, help="directory to write the asset files to")
    p.add_argument("--cache", type=Path, default=Path("firmware/.pio/fontcache"))
    args = p.parse_args(argv)
    assets = build_assets(args.cache)
    if args.out:
        write_files(assets, args.out)
    if args.header:
        total = write_header(assets, args.header)
        print(f"{args.header}: {len(assets)} assets, {total // 1024} KB in flash")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
