/*
 * bookprep.js: EPUB/TXT -> .t5b in the browser.
 *
 * A port of tools/bookprep (Python): same extraction, normalisation, pyphen
 * hyphenation, line breaking, pagination and file format. Keep the two in step;
 * tools/bookprep/tests/test_webport.py (Node) and test_web_e2e.py (headless
 * Chromium) check that they agree. Rendering uses a canvas, so glyphs come from
 * the browser's font engine instead of FreeType.
 *
 * Works in browsers and in Node; EPUB parsing needs DOMParser, rendering needs
 * a canvas (both browser-only).
 */
(function (root, factory) {
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  else root.BookPrep = api;
})(typeof self !== "undefined" ? self : this, function () {
  "use strict";

  const ch = String.fromCharCode;
  const NBSP = ch(0xa0);
  const LINE_BREAK = ch(0x2028); // placeholder for <br> while whitespace is collapsed
  const RSQUO = "’";
  const LSQUO = "‘";
  const LDQUO = "“";
  const RDQUO = "”";
  const ELLIPSIS = "…";
  const EM_DASH = "—";
  const EN_DASH = "–";
  const SCENE_BREAK = "* * *";
  const WORD = "\\p{L}\\p{N}_"; // Python's Unicode \w

  // --- geometry and options (bookprep defaults) -------------------------------------

  // Readers: page size and the converter defaults that suit it. Same as SCREENS in
  // tools/bookprep/src/bookprep/layout.py and firmware/include/boards/*.h; the reader
  // reports its own screen in /api/info.
  const screens = {
    "t5-213": { id: "t5-213", name: "LilyGo T5-2.13", width: 250, height: 122, statusBar: 14, size: 13, margin: 4, marginBottom: 4 },
    "4.7in": { id: "4.7in", name: "LilyGo T5-4.7\" S3, M5PaperS3, M5Paper", width: 540, height: 960, statusBar: 28, size: 28, margin: 24, marginBottom: 8 },
  };

  // A screen from /api/info (or an id): known fields filled in from the table.
  function screenFor(sc) {
    if (typeof sc === "string") sc = { id: sc };
    sc = sc || {};
    if (screens[sc.id]) return { ...screens[sc.id], ...sc };
    // a reader this page doesn't know: scale the bottom margin with the status bar
    const out = { ...screens["t5-213"], ...sc };
    if (sc.marginBottom === undefined) out.marginBottom = Math.round(out.statusBar * 4 / 14);
    return out;
  }

  function geometry(o = {}) {
    const g = {
      width: 250, height: 122, marginLeft: 4, marginRight: 4, marginTop: 4,
      marginBottom: 4, statusBar: 14, ...o,
    };
    g.textLeft = g.marginLeft;
    g.textTop = g.marginTop;
    g.textWidth = g.width - g.marginLeft - g.marginRight;
    g.textHeight = g.height - g.marginTop - g.marginBottom - g.statusBar;
    return g;
  }

  // Line height in px for a font size and spacing factor (bookprep: ceil(size * 1.2)).
  function lineHeightFor(size, spacing = 1.2) {
    return Math.ceil(size * spacing - 1e-6);
  }

  const defaults = {
    font: "ubuntu", size: 13, spacing: 1.2, margin: 4, indent: true, paragraphSpacing: 0,
    justify: true, hyphenate: true, lang: "auto", threshold: 128, chapterPageBreak: true,
    widowOrphan: true,
  };

  // --- ZIP (EPUB container) ---------------------------------------------------------

  async function inflateRaw(bytes) {
    const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream("deflate-raw"));
    return new Uint8Array(await new Response(stream).arrayBuffer());
  }

  async function openZip(buffer) {
    const bytes = new Uint8Array(buffer);
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    let eocd = -1;
    for (let i = bytes.length - 22; i >= Math.max(0, bytes.length - 65557); i--) {
      if (dv.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
    }
    if (eocd < 0) throw new Error("not a ZIP/EPUB file");
    const count = dv.getUint16(eocd + 10, true);
    let p = dv.getUint32(eocd + 16, true);
    const entries = new Map();
    const utf8 = new TextDecoder("utf-8");
    for (let n = 0; n < count; n++) {
      if (dv.getUint32(p, true) !== 0x02014b50) throw new Error("corrupt ZIP directory");
      const method = dv.getUint16(p + 10, true);
      const compSize = dv.getUint32(p + 20, true);
      const nameLen = dv.getUint16(p + 28, true);
      const extraLen = dv.getUint16(p + 30, true);
      const commentLen = dv.getUint16(p + 32, true);
      const local = dv.getUint32(p + 42, true);
      const name = utf8.decode(bytes.subarray(p + 46, p + 46 + nameLen));
      entries.set(name, { method, compSize, local });
      p += 46 + nameLen + extraLen + commentLen;
    }
    async function read(name) {
      const e = entries.get(name);
      if (!e) throw new Error("missing in EPUB: " + name);
      const start = e.local + 30 + dv.getUint16(e.local + 26, true) + dv.getUint16(e.local + 28, true);
      const data = bytes.subarray(start, start + e.compSize);
      if (e.method === 0) return data;
      if (e.method === 8) return inflateRaw(data);
      throw new Error("unsupported ZIP compression " + e.method);
    }
    return {
      names: () => [...entries.keys()],
      has: (name) => entries.has(name),
      read,
      text: async (name) => utf8.decode(await read(name)),
    };
  }

  // --- paths --------------------------------------------------------------------------

  function dirname(path) {
    const i = path.lastIndexOf("/");
    return i < 0 ? "" : path.slice(0, i + 1);
  }

  // Resolves href (relative to base dir) to a normalised path inside the ZIP.
  function resolvePath(base, href) {
    let path = href;
    try { path = decodeURIComponent(href); } catch (e) { /* keep as is */ }
    const out = [];
    for (const part of (base + path).split("/")) {
      if (part === "..") out.pop();
      else if (part !== "." && part !== "") out.push(part);
    }
    return out.join("/");
  }

  // --- extraction ---------------------------------------------------------------------

  const HEADING_TAGS = new Set(["h1", "h2", "h3", "h4", "h5", "h6"]);
  const IMAGE_TAGS = new Set(["img", "svg", "image"]);
  const SKIP_TAGS = new Set(["head", "script", "style", "title", "meta", "link", "noscript"]);
  const BLOCK_TAGS = new Set([
    "address", "article", "aside", "blockquote", "body", "dd", "div", "dl", "dt",
    "figcaption", "figure", "footer", "header", "li", "main", "nav", "ol", "p",
    "pre", "section", "table", "tbody", "td", "tfoot", "th", "thead", "tr", "ul",
  ]);
  const FOOTNOTE_REF = /\s*\[\d+\]$/;

  function collapse(text) {
    return text.replace(/[ \t\r\n\f]+/g, " ").trim();
  }

  function cleanInline(text) {
    const lines = collapse(text).split(LINE_BREAK).map((l) => l.trim());
    return lines.join("\n").replace(/^\n+|\n+$/g, "");
  }

  function textContent(node, sep) {
    const parts = [];
    (function walk(n) {
      for (const c of n.childNodes) {
        if (c.nodeType === 3 || c.nodeType === 4) parts.push(c.nodeValue);
        else if (c.nodeType === 1) walk(c);
      }
    })(node);
    return parts.join(sep);
  }

  // Blocks of one (X)HTML document; anchors maps element ids to TOC titles.
  function htmlToBlocks(html, anchors = new Map()) {
    anchors = new Map(anchors);
    const doc = new DOMParser().parseFromString(html, "text/html");
    const root = doc.body || doc.documentElement;
    const blocks = [];
    let buffer = [];
    const flush = () => {
      const text = cleanInline(buffer.join(""));
      buffer = [];
      if (text) blocks.push({ kind: "text", text });
    };
    (function walk(node) {
      for (const child of node.childNodes) {
        if (child.nodeType === 3 || child.nodeType === 4) { buffer.push(child.nodeValue); continue; }
        if (child.nodeType !== 1) continue;
        const name = child.localName.toLowerCase().split(":").pop();
        if (SKIP_TAGS.has(name)) continue;
        for (const key of [child.getAttribute("id"), child.getAttribute("name")]) {
          if (key !== null && anchors.has(key)) {
            flush();
            blocks.push({ kind: "anchor", text: anchors.get(key) });
            anchors.delete(key);
          }
        }
        if (name === "br") buffer.push(LINE_BREAK);
        else if (name === "hr") { flush(); blocks.push({ kind: "break", text: "" }); }
        else if (IMAGE_TAGS.has(name)) { flush(); blocks.push({ kind: "image", text: "[image]" }); }
        else if (HEADING_TAGS.has(name)) {
          flush();
          let text = cleanInline(textContent(child, " ")).replace(/\n/g, " ");
          text = text.replace(FOOTNOTE_REF, "");
          if (text) blocks.push({ kind: "heading", text });
        } else if (BLOCK_TAGS.has(name)) { flush(); walk(child); flush(); }
        else walk(child);
      }
    })(root);
    flush();
    return blocks;
  }

  function parseXml(text) {
    return new DOMParser().parseFromString(text, "application/xml");
  }

  function byLocalName(node, name) {
    return [...node.getElementsByTagName("*")].filter((e) => e.localName === name);
  }

  // Flattened TOC as [docPath, fragment|null, title], NCX first, else EPUB 3 nav.
  async function tocTargets(zip, manifest, spineToc) {
    const targets = [];
    const add = (base, href, title) => {
      title = collapse(title || "").replace(FOOTNOTE_REF, "");
      if (!href || !title) return;
      const hash = href.indexOf("#");
      const doc = resolvePath(base, hash < 0 ? href : href.slice(0, hash));
      let fragment = hash < 0 ? null : href.slice(hash + 1);
      try { if (fragment) fragment = decodeURIComponent(fragment); } catch (e) { /* keep */ }
      targets.push([doc, fragment || null, title]);
    };
    const items = [...manifest.values()];
    const ncx = (spineToc && manifest.get(spineToc)) || items.find((i) => i.mediaType === "application/x-dtbncx+xml");
    if (ncx && zip.has(ncx.path)) {
      const doc = parseXml(await zip.text(ncx.path));
      for (const point of byLocalName(doc, "navPoint")) {
        const label = byLocalName(point, "text")[0];
        const content = byLocalName(point, "content")[0];
        add(dirname(ncx.path), content && content.getAttribute("src"), label && label.textContent);
      }
      return targets;
    }
    const nav = items.find((i) => (i.properties || "").split(/\s+/).includes("nav"));
    if (nav && zip.has(nav.path)) {
      const doc = new DOMParser().parseFromString(await zip.text(nav.path), "text/html");
      const navs = [...doc.getElementsByTagName("nav")];
      const toc = navs.find((n) => (n.getAttribute("epub:type") || "").includes("toc")) || navs[0];
      if (toc) for (const a of toc.getElementsByTagName("a")) add(dirname(nav.path), a.getAttribute("href"), a.textContent);
    }
    return targets;
  }

  async function extractEpub(buffer, fileName = "book.epub") {
    const zip = await openZip(buffer);
    const container = parseXml(await zip.text("META-INF/container.xml"));
    const rootfile = byLocalName(container, "rootfile")[0];
    if (!rootfile) throw new Error("EPUB has no rootfile");
    const opfPath = rootfile.getAttribute("full-path");
    const opfDir = dirname(opfPath);
    const opf = parseXml(await zip.text(opfPath));
    const meta = (name) => byLocalName(opf, name).map((e) => e.textContent.trim()).filter(Boolean);

    const manifest = new Map();
    for (const item of byLocalName(opf, "item")) {
      manifest.set(item.getAttribute("id"), {
        path: resolvePath(opfDir, item.getAttribute("href") || ""),
        mediaType: item.getAttribute("media-type") || "",
        properties: item.getAttribute("properties") || "",
      });
    }
    const spineEl = byLocalName(opf, "spine")[0];
    const targets = await tocTargets(zip, manifest, spineEl && spineEl.getAttribute("toc"));
    const docTitles = new Map();
    const anchors = new Map();
    for (const [doc, fragment, title] of targets) {
      if (fragment === null) { if (!docTitles.has(doc)) docTitles.set(doc, title); }
      else {
        if (!anchors.has(doc)) anchors.set(doc, new Map());
        if (!anchors.get(doc).has(fragment)) anchors.get(doc).set(fragment, title);
      }
    }

    // Chapters start where the TOC points (a document or an anchor in one);
    // documents the TOC doesn't point at continue the previous chapter.
    let chapters = [];
    for (const ref of spineEl ? byLocalName(spineEl, "itemref") : []) {
      if (ref.getAttribute("linear") === "no") continue;
      const item = manifest.get(ref.getAttribute("idref"));
      if (!item || !zip.has(item.path)) continue;
      if (!/^(application\/xhtml\+xml|text\/html)$/.test(item.mediaType)) continue;
      if (item.properties.split(/\s+/).includes("nav")) continue;
      const blocks = htmlToBlocks(await zip.text(item.path), anchors.get(item.path) || new Map());
      if (!blocks.some((b) => b.kind === "text" || b.kind === "heading")) continue;
      if (docTitles.has(item.path) || !chapters.length || !targets.length) {
        let title = docTitles.get(item.path) || null;
        if (title === null && !targets.length) {
          const h = blocks.find((b) => b.kind === "heading");
          title = h ? h.text : null;
        }
        chapters.push({ title, blocks: [] });
      }
      for (const b of blocks) {
        if (b.kind === "anchor") chapters.push({ title: b.text, blocks: [] });
        else chapters[chapters.length - 1].blocks.push(b);
      }
    }
    chapters = chapters.filter((c) => c.blocks.some((b) => b.kind === "text" || b.kind === "heading"));
    const titles = meta("title");
    const languages = meta("language");
    return {
      title: titles[0] || fileName.replace(/\.[^.]*$/, ""),
      author: meta("creator").join(", "),
      language: languages[0] || null,
      chapters,
    };
  }

  const GUTENBERG_START = /^\*\*\* ?START OF (THE|THIS) PROJECT GUTENBERG.*$/im;
  const GUTENBERG_END = /^\*\*\* ?END OF (THE|THIS) PROJECT GUTENBERG.*$/im;
  const CHAPTER_HEADING = /^(chapter|hoofdstuk|part|deel|book|boek|prologue|proloog|epilogue|epiloog)(?![\p{L}\p{N}_]).{0,60}$/iu;
  const ROMAN_HEADING = /^[IVXLC]{1,7}\.?$/;

  function decodeText(bytes) {
    try {
      let text = new TextDecoder("utf-8", { fatal: true }).decode(bytes);
      if (text.charCodeAt(0) === 0xfeff) text = text.slice(1);
      return text;
    } catch (e) {
      return new TextDecoder("windows-1252").decode(bytes);
    }
  }

  function extractTxt(buffer, fileName = "book.txt") {
    let text = decodeText(new Uint8Array(buffer)).replace(/\r\n?/g, "\n");
    const start = GUTENBERG_START.exec(text);
    if (start) text = text.slice(start.index + start[0].length);
    const end = GUTENBERG_END.exec(text);
    if (end) text = text.slice(0, end.index);
    let chapters = [{ title: null, blocks: [] }];
    for (const raw of text.split(/\n\s*\n/)) {
      const para = collapse(raw);
      if (!para) continue;
      if (CHAPTER_HEADING.test(para) || ROMAN_HEADING.test(para)) {
        chapters.push({ title: para, blocks: [{ kind: "heading", text: para }] });
      } else {
        chapters[chapters.length - 1].blocks.push({ kind: "text", text: para });
      }
    }
    chapters = chapters.filter((c) => c.blocks.length);
    return { title: fileName.replace(/\.[^.]*$/, ""), author: "", language: null, chapters };
  }

  async function extract(buffer, fileName) {
    if (/\.epub$/i.test(fileName)) return extractEpub(buffer, fileName);
    if (/\.txt$/i.test(fileName)) return extractTxt(buffer, fileName);
    throw new Error("unsupported file: expected .epub or .txt");
  }

  // --- normalisation ------------------------------------------------------------------

  const REMOVE = new RegExp("[" + [0xad, 0x200b, 0x200c, 0x200d, 0x2060, 0xfeff].map((c) => ch(c)).join("") + "]", "g");
  const SPACES = new RegExp("[" + [0x09, 0x2002, 0x2003, 0x2004, 0x2005, 0x2006, 0x2008, 0x2009, 0x200a, 0x3000].map((c) => ch(c)).join("") + "]", "g");
  const NB_SPACES = new RegExp("[" + ch(0x2007) + ch(0x202f) + "]", "g");
  const ELISION = new RegExp("(?<![" + WORD + RSQUO + "])'(?=(?:s|t|n|k|em|r)(?![" + WORD + "])|[0-9][0-9](?![" + WORD + "]))", "giu");
  const OPEN = "(?:^|(?<=[\\s([{" + EM_DASH + EN_DASH + "/-]))";
  const OPEN_DOUBLE = new RegExp(OPEN + '"', "gmu");
  const OPEN_SINGLE = new RegExp(OPEN + "'", "gmu");

  function smartQuotes(text) {
    text = text.replace(ELISION, RSQUO);
    text = text.replace(OPEN_DOUBLE, LDQUO).replaceAll('"', RDQUO);
    text = text.replace(OPEN_SINGLE, LSQUO).replaceAll("'", RSQUO);
    return text;
  }

  function normalizeText(text) {
    text = text.normalize("NFC").replace(REMOVE, "").replace(SPACES, " ").replace(NB_SPACES, NBSP);
    text = text.replace(/\.\s?\.\s?\./g, ELLIPSIS);
    text = text.replace(/(?<!-)--(?!-)/g, EM_DASH);
    return smartQuotes(text);
  }

  function cleanBook(book) {
    const n = (t) => normalizeText(t);
    return {
      ...book,
      title: n(book.title),
      author: n(book.author),
      chapters: book.chapters.map((c) => ({
        title: c.title ? n(c.title) : null,
        blocks: c.blocks.map((b) => ({ kind: b.kind, text: n(b.text) })),
      })),
    };
  }

  // --- hyphenation (pyphen) ---------------------------------------------------------

  // Patterns as exported by bookprep.webassets: "maxlen N", then "key start values".
  function parsePatterns(text) {
    const lines = text.split("\n");
    const maxlen = parseInt(lines[0].split(" ")[1], 10);
    const patterns = new Map();
    for (let i = 1; i < lines.length; i++) {
      const line = lines[i];
      if (!line) continue;
      const a = line.indexOf(" ");
      const b = line.indexOf(" ", a + 1);
      const values = line.slice(b + 1).split("").map(Number);
      patterns.set(line.slice(0, a), [parseInt(line.slice(a + 1, b), 10), values]);
    }
    return { maxlen, patterns };
  }

  // pyphen.language_fallback over the bundled dictionaries.
  function languageFallback(code, aliases) {
    if (!code) return null;
    const parts = code.replace(/-/g, "_").toLowerCase().split("_");
    while (parts.length) {
      const name = parts.join("_");
      if (aliases[name]) return aliases[name];
      parts.pop();
    }
    return null;
  }

  const BREAK_AFTER = "-" + ch(0x2010) + ch(0x2011) + EN_DASH + EM_DASH;
  const CORE = new RegExp("^([^" + WORD + "]*)([\\s\\S]*?)([^" + WORD + "]*)$", "u");
  const HYPHENATABLE = new RegExp("^[\\p{L}'" + RSQUO + "]+$", "u");

  class Hyphenator {
    constructor(dict, { left = 2, right = 3, minWord = 5 } = {}) {
      this.dict = dict;
      this.left = left;
      this.right = right;
      this.minWord = minWord;
      this.cache = new Map();
    }

    positions(word) {
      const lower = word.toLowerCase();
      let points = this.cache.get(lower);
      if (!points) {
        const pointed = "." + lower + ".";
        const refs = new Array(pointed.length + 1).fill(0);
        const { maxlen, patterns } = this.dict;
        for (let i = 0; i < pointed.length - 1; i++) {
          const stop = Math.min(i + maxlen, pointed.length) + 1;
          for (let j = i + 1; j < stop; j++) {
            const pattern = patterns.get(pointed.slice(i, j));
            if (!pattern) continue;
            const [offset, values] = pattern;
            for (let k = 0; k < values.length; k++) {
              const at = i + offset + k;
              if (values[k] > refs[at]) refs[at] = values[k];
            }
          }
        }
        points = [];
        refs.forEach((r, i) => { if (r % 2) points.push(i - 1); });
        this.cache.set(lower, points);
      }
      const right = word.length - this.right;
      return points.filter((i) => this.left <= i && i <= right);
    }

    // Dictionary breaks as [head + "-", tail], longest head first.
    splits(word) {
      const [, lead, core, trail] = CORE.exec(word);
      if (core.length < this.minWord || !HYPHENATABLE.test(core)) return [];
      return this.positions(core).reverse().map((p) => [lead + core.slice(0, p) + "-", core.slice(p) + trail]);
    }
  }

  function dashBreaks(word) {
    const out = [];
    for (let i = 1; i < word.length - 1; i++) {
      if (BREAK_AFTER.includes(word[i])) out.push([word.slice(0, i + 1), word.slice(i + 1)]);
    }
    return out;
  }

  // --- line breaking ----------------------------------------------------------------

  const round2 = (x) => Math.round(x * 100) / 100;

  function fitSplit(word, room, measure, hyphenator) {
    if (room <= 0) return null;
    const candidates = dashBreaks(word).concat(hyphenator ? hyphenator.splits(word) : []);
    candidates.sort((a, b) => b[0].length - a[0].length);
    for (const [head, tail] of candidates) if (measure(head) <= room) return [head, tail];
    return null;
  }

  function forceSplit(word, avail, measure) {
    let cut = 1;
    while (cut < word.length && measure(word.slice(0, cut + 1)) <= avail) cut++;
    return [word.slice(0, cut), word.slice(cut)];
  }

  // One paragraph -> lines of positioned word runs ({x, text}); "\n" forces a break.
  function breakLines(text, measure, width, { indent = 0, hyphenator = null, justify = false, align = "left" } = {}) {
    const space = measure(" ");
    const lines = [];
    const emit = (words, offset, avail, stretch) => {
      const widths = words.map((w) => measure(w));
      const natural = widths.reduce((a, b) => a + b, 0) + space * Math.max(words.length - 1, 0);
      let gap = space;
      if (stretch && words.length > 1) gap = space + (avail - natural) / (words.length - 1);
      else if (align === "center") offset += (avail - natural) / 2;
      let x = offset;
      lines.push(words.map((w, i) => {
        const run = { x: round2(x), text: w };
        x += widths[i] + gap;
        return run;
      }));
    };
    for (const segment of text.split("\n")) {
      const queue = segment.split(" ").filter(Boolean);
      if (!queue.length) { emit([], 0, width, false); continue; }
      let current = [];
      let used = 0;
      while (queue.length) {
        const offset = lines.length ? 0 : indent;
        const avail = width - offset;
        const word = queue.shift();
        const w = measure(word);
        const needed = current.length ? used + space + w : w;
        if (needed <= avail) { current.push(word); used = needed; continue; }
        const room = avail - (current.length ? used + space : 0);
        let split = fitSplit(word, room, measure, hyphenator);
        if (split === null && !current.length) split = forceSplit(word, avail, measure);
        if (split) { current.push(split[0]); queue.unshift(split[1]); }
        else queue.unshift(word);
        emit(current, offset, avail, justify);
        current = [];
        used = 0;
      }
      const offset = lines.length ? 0 : indent;
      emit(current, offset, width - offset, false);
    }
    return lines;
  }

  // --- pagination ---------------------------------------------------------------------

  function bookItems(chapters, measure, g, opts, hyphenator) {
    const width = g.textWidth;
    const lh = opts.lineHeight;
    const items = [];
    let para = 0;
    const add = (runsList, chapter, keep = false) => {
      runsList.forEach((runs, i) => items.push({ type: "line", runs, chapter, para, index: i, count: runsList.length, keep }));
      para++;
    };
    chapters.forEach((chapter, ci) => {
      if (opts.chapterPageBreak && items.length) items.push({ type: "break" });
      let prev = null;
      for (const block of chapter.blocks) {
        if (block.kind === "heading") {
          if (prev !== null) items.push({ type: "gap", height: Math.floor(lh / 2) });
          add(breakLines(block.text, measure, width, { align: "center" }), ci, true);
          items.push({ type: "gap", height: Math.floor(lh / 4) });
        } else if (block.kind === "text") {
          if (prev !== null && opts.paragraphSpacing) items.push({ type: "gap", height: opts.paragraphSpacing });
          const indent = prev === "text" ? opts.indent : 0;
          add(breakLines(block.text, measure, width, { indent, hyphenator, justify: opts.justify }), ci);
        } else {
          const text = block.kind === "break" ? SCENE_BREAK : block.text;
          add(breakLines(text, measure, width, { align: "center" }), ci);
        }
        prev = block.kind;
      }
    });
    return items;
  }

  function pageEnd(lines, nxt) {
    let k = lines.length;
    const last = lines[k - 1].line;
    if (nxt.index === nxt.count - 1 && nxt.count > 1 && last.para === nxt.para) k--;
    if (k && lines[k - 1].line.index === 0 && lines[k - 1].line.count > 1) k--;
    while (k && lines[k - 1].line.keep) k--;
    return k > 0 ? k : lines.length;
  }

  function paginate(items, height, lineHeight, widowOrphan = true) {
    const pages = [];
    let page = [];
    let y = 0;
    const flush = () => {
      if (page.length) pages.push(page);
      page = [];
      y = 0;
    };
    let i = 0;
    while (i < items.length) {
      const item = items[i];
      if (item.type === "break") { flush(); i++; }
      else if (item.type === "gap") { if (page.length) y += item.height; i++; }
      else if (y + lineHeight <= height || !page.length) { page.push({ y, line: item }); y += lineHeight; i++; }
      else {
        const keep = widowOrphan ? pageEnd(page, item) : page.length;
        const carried = page.slice(keep);
        page = page.slice(0, keep);
        const base = carried.length ? carried[0].y : 0;
        const end = y;
        flush();
        page = carried.map((p) => ({ y: p.y - base, line: p.line }));
        y = carried.length ? end - base : 0;
      }
    }
    flush();
    return pages;
  }

  function layoutBook(chapters, measure, g, opts, hyphenator) {
    return paginate(bookItems(chapters, measure, g, opts, hyphenator), g.textHeight, opts.lineHeight, opts.widowOrphan);
  }

  function tocEntries(chapters, pages) {
    const first = new Map();
    pages.forEach((page, n) => page.forEach((p) => { if (!first.has(p.line.chapter)) first.set(p.line.chapter, n); }));
    const toc = [];
    chapters.forEach((c, ci) => { if (c.title && first.has(ci)) toc.push({ startPage: first.get(ci), title: c.title }); });
    return toc;
  }

  // --- .t5b ---------------------------------------------------------------------------

  const HEADER_SIZE = 146;
  const TOC_ENTRY_SIZE = 52;
  const strideFor = (w) => Math.floor((w + 7) / 8);

  function encodeStr(text, size) {
    const bytes = new TextEncoder().encode(text);
    let n = Math.min(bytes.length, size - 1);
    while (n > 0 && n < bytes.length && (bytes[n] & 0xc0) === 0x80) n--;
    const out = new Uint8Array(size);
    out.set(bytes.subarray(0, n));
    return out;
  }

  function writeT5b({ title, author, pageW, pageH, toc, pages }) {
    const pageBytes = strideFor(pageW) * pageH;
    toc = toc.slice(0, 0xffff);
    const out = new Uint8Array(HEADER_SIZE + toc.length * TOC_ENTRY_SIZE + pages.length * pageBytes);
    const dv = new DataView(out.buffer);
    out.set([0x54, 0x35, 0x42, 0x31], 0); // "T5B1"
    dv.setUint16(4, 1, true);
    dv.setUint16(6, pageW, true);
    dv.setUint16(8, pageH, true);
    dv.setUint16(10, strideFor(pageW), true);
    dv.setUint32(12, pages.length, true);
    dv.setUint16(16, toc.length, true);
    out.set(encodeStr(title, 64), 18);
    out.set(encodeStr(author, 64), 82);
    let p = HEADER_SIZE;
    for (const e of toc) {
      dv.setUint32(p, e.startPage, true);
      out.set(encodeStr(e.title, 48), p + 4);
      p += TOC_ENTRY_SIZE;
    }
    for (const page of pages) {
      if (page.length !== pageBytes) throw new Error("page has the wrong size");
      out.set(page, p);
      p += pageBytes;
    }
    return out;
  }

  // RGBA pixels -> 1-bpp page (MSB first, 1 = white, padding bits 1).
  function packPixels(rgba, w, h, threshold) {
    const stride = strideFor(w);
    const out = new Uint8Array(stride * h).fill(0xff);
    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        const i = (y * w + x) * 4;
        const lum = (rgba[i] * 299 + rgba[i + 1] * 587 + rgba[i + 2] * 114) / 1000;
        if (lum < threshold) out[y * stride + (x >> 3)] &= ~(0x80 >> (x & 7));
      }
    }
    return out;
  }

  // --- rendering (browser) ----------------------------------------------------------

  // Canvas renderer for one font/size. family must already be loaded (FontFace).
  function createRenderer(family, size, g, lineHeight, threshold) {
    const canvas = typeof OffscreenCanvas !== "undefined" ? new OffscreenCanvas(g.width, g.height) : Object.assign(document.createElement("canvas"), { width: g.width, height: g.height });
    const ctx = canvas.getContext("2d", { willReadFrequently: true });
    const font = `${size}px "${family}", sans-serif`;
    ctx.font = font;
    const m = ctx.measureText("Hg");
    const ascent = Math.ceil(m.fontBoundingBoxAscent || m.actualBoundingBoxAscent);
    const descent = Math.ceil(m.fontBoundingBoxDescent || m.actualBoundingBoxDescent);
    const baseline = Math.floor((lineHeight - (ascent + descent)) / 2) + ascent;
    const cache = new Map();
    const measure = (s) => {
      let w = cache.get(s);
      if (w === undefined) {
        ctx.font = font;
        w = ctx.measureText(s.replaceAll(NBSP, " ")).width;
        cache.set(s, w);
      }
      return w;
    };
    function draw(page) {
      ctx.fillStyle = "#fff";
      ctx.fillRect(0, 0, g.width, g.height);
      ctx.fillStyle = "#000";
      ctx.font = font;
      ctx.textBaseline = "alphabetic";
      for (const { y, line } of page) {
        for (const run of line.runs) ctx.fillText(run.text.replaceAll(NBSP, " "), g.textLeft + run.x, g.textTop + y + baseline);
      }
      ctx.fillStyle = "#fff";
      ctx.fillRect(0, g.height - g.statusBar, g.width, g.statusBar);
      return ctx.getImageData(0, 0, g.width, g.height).data;
    }
    return {
      measure,
      canvas,
      render: (page) => packPixels(draw(page), g.width, g.height, threshold),
    };
  }

  // Unpacked 1-bpp page -> RGBA (for previews).
  function unpackToRgba(page, w, h) {
    const stride = strideFor(w);
    const rgba = new Uint8ClampedArray(w * h * 4);
    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        const v = page[y * stride + (x >> 3)] & (0x80 >> (x & 7)) ? 255 : 0;
        const i = (y * w + x) * 4;
        rgba[i] = rgba[i + 1] = rgba[i + 2] = v;
        rgba[i + 3] = 255;
      }
    }
    return rgba;
  }

  // Lays out a cleaned book with the given options and font family, for
  // opts.screen (a screen object or id; default t5-213).
  function typeset(book, opts, family, hyphenator) {
    const o = { ...defaults, ...opts };
    const sc = screenFor(o.screen);
    const g = geometry({
      width: sc.width, height: sc.height, statusBar: sc.statusBar, marginBottom: sc.marginBottom,
      marginLeft: o.margin, marginRight: o.margin, marginTop: o.margin,
    });
    const lineHeight = lineHeightFor(o.size, o.spacing);
    const renderer = createRenderer(family, o.size, g, lineHeight, o.threshold);
    const layoutOpts = {
      lineHeight,
      indent: o.indent ? o.size : 0,
      paragraphSpacing: o.paragraphSpacing,
      justify: o.justify,
      chapterPageBreak: o.chapterPageBreak,
      widowOrphan: o.widowOrphan,
    };
    const pages = layoutBook(book.chapters, renderer.measure, g, layoutOpts, o.hyphenate ? hyphenator : null);
    const toc = tocEntries(book.chapters, pages);
    return { pages, toc, renderer, geometry: g, lineHeight };
  }

  // Renders every page into a .t5b, yielding to the event loop now and then.
  async function writeBook(book, layout, onProgress) {
    const bitmaps = [];
    for (let n = 0; n < layout.pages.length; n++) {
      bitmaps.push(layout.renderer.render(layout.pages[n]));
      if (n % 25 === 24) {
        if (onProgress) onProgress(n + 1, layout.pages.length);
        await new Promise((r) => setTimeout(r, 0));
      }
    }
    if (onProgress) onProgress(layout.pages.length, layout.pages.length);
    return writeT5b({
      title: book.title, author: book.author, pageW: layout.geometry.width,
      pageH: layout.geometry.height, toc: layout.toc, pages: bitmaps,
    });
  }

  // bookprep's file-name slug.
  function slug(name) {
    const s = name.replace(/[^A-Za-z0-9._-]+/g, "_").replace(/^_+|_+$/g, "");
    return s || "book";
  }

  return {
    defaults, screens, screenFor, geometry, lineHeightFor, openZip, resolvePath, htmlToBlocks, extractEpub, extractTxt,
    extract, normalizeText, cleanBook, parsePatterns, languageFallback, Hyphenator, dashBreaks,
    breakLines, bookItems, paginate, layoutBook, tocEntries, encodeStr, writeT5b, packPixels,
    createRenderer, unpackToRgba, typeset, writeBook, slug, NBSP,
  };
});
