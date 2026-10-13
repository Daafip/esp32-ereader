// Runs the parity cases from tools/bookprep/tests/test_webport.py through
// bookprep.js and prints the results as JSON. Usage: node port_check.js cases.json
"use strict";
const fs = require("fs");
const path = require("path");
const B = require(path.join(__dirname, "..", "bookprep.js"));

const cases = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
const mono = (s) => 6 * [...s].length;
const out = {};

out.normalize = cases.normalize.map((t) => B.normalizeText(t));

const dicts = {};
for (const [lang, text] of Object.entries(cases.patterns)) dicts[lang] = new B.Hyphenator(B.parsePatterns(text));
out.splits = cases.splits.map(([lang, word]) => dicts[lang].splits(word));
out.languages = cases.languages.map((code) => B.languageFallback(code, cases.aliases));

out.lines = cases.lines.map((c) =>
  B.breakLines(c.text, mono, c.width, {
    indent: c.indent,
    justify: c.justify,
    align: c.align,
    hyphenator: c.lang ? dicts[c.lang] : null,
  }));

out.layout = cases.layout.map((c) => {
  let g = B.geometry(c.geometry);
  if (c.geometry.screen) {  // as typeset() does it for a reader's screen
    const sc = B.screenFor(c.geometry.screen);
    const m = c.geometry.margin;
    g = B.geometry({ width: sc.width, height: sc.height, statusBar: sc.statusBar,
      marginBottom: sc.marginBottom, marginLeft: m, marginRight: m, marginTop: m });
  }
  const pages = B.layoutBook(c.chapters, mono, g, c.options, c.lang ? dicts[c.lang] : null);
  return {
    pages: pages.map((p) => p.map(({ y, line }) => [y, line.runs.map((r) => r.text).join(" ")])),
    toc: B.tocEntries(c.chapters, pages).map((e) => [e.startPage, e.title]),
  };
});

out.txt = cases.txt.map(([name, b64]) => B.extractTxt(Buffer.from(b64, "base64"), name));

out.t5b = Buffer.from(B.writeT5b({
  ...cases.t5b,
  pages: cases.t5b.pages.map((b64) => new Uint8Array(Buffer.from(b64, "base64"))),
  cover: { ...cases.t5b.cover, bits: new Uint8Array(Buffer.from(cases.t5b.cover.bits, "base64")) },
})).toString("base64");
out.bionic = cases.bionic.map((w) => B.bionicPrefix(w));
out.dither = cases.dither.map((c) =>
  Buffer.from(B.dither(c.gray, c.w, c.h, { method: c.method, level: c.level })).toString("base64"));
out.sharpen = Array.from(B.sharpen(cases.sharpen.gray, cases.sharpen.w, cases.sharpen.h));

out.encode = cases.encode.map(([text, size]) => Buffer.from(B.encodeStr(text, size)).toString("base64"));
out.slug = cases.slug.map((s) => B.slug(s));
out.lineHeights = cases.lineHeights.map(([size, spacing]) => B.lineHeightFor(size, spacing));
out.screens = B.screens;

(async () => {
  // ZIP reader + inflate on a real EPUB (no DOM needed for this part)
  const zip = await B.openZip(fs.readFileSync(cases.zip.path));
  out.zip = { names: zip.names().sort(), container: await zip.text("META-INF/container.xml") };
  process.stdout.write(JSON.stringify(out));
})();
