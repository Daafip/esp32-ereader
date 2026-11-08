# .t5b book format (version 2)

Written by `tools/bookprep` (`bookprep/t5b.py` is the reference implementation) and
the web page's converter (`firmware/web/bookprep.js`), read by the firmware
(`lib/core/src/t5b_format.*`). Everything is little-endian. Strings are UTF-8,
NUL-padded and always NUL-terminated within their field.

**Versions.** Version 2 (firmware 2.0.0, 2026-11-08) stores each page as a zlib
stream behind a page index: about 5 times smaller for text (a 797-page novel on the
7.5": 38.3 MB in version 1, 7.9 MB in version 2). Version 1 stores the pages
uncompressed. The firmware reads both; firmware 1.x only reads version 1
(`pixi run prep --uncompressed` still writes it). The browser converter always
matches the firmware that serves it.

## Layout (version 2)

| Offset | Size | Field |
|---|---|---|
| 0 | 146 | header, as in version 1 below, with version = 2 |
| 146 | 52 × toc_count | TOC entries, as in version 1 |
| I = 146 + 52 × toc_count | 4 × (page_count + 1) | page index: u32 file offset of each page's data, then the end of the last page |
| index[0] = I + 4 × (page_count + 1) | index[page_count] − index[0] | pages: page *n* is the bytes index[n] to index[n+1] |
| index[page_count] | 12 + cover stride × cover h | optional cover block, as in version 1 (uncompressed) |

Each page is exactly one zlib stream (RFC 1950 around RFC 1951 deflate, no preset
dictionary) that inflates to `page_bytes`, the bitmap below. The stream fills its
stretch of the file exactly; offsets increase strictly. Python writes level 9
(`zlib.compress`), the browser its `CompressionStream("deflate")` default: both
inflate to the same pages.

A reader checks `index[0]` and `index[page_count]` (at most the file size; a cover
follows it or the file ends there) when it opens a book, and a page's two entries
when it reads that page. The firmware inflates a page straight into the framebuffer
with its own small inflater (`lib/core/src/inflater.*`: no 32 KB window, since
back-references point into the page itself; about 1.8 KB of state). The Wi-Fi
page preview (`GET /api/page`) sends a page as stored, with `Content-Encoding:
deflate`, and the browser inflates it.

## Layout (version 1)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `"T5B1"` |
| 4 | 2 | version (u16) = 1 (2: compressed, above) |
| 6 | 2 | page_w (u16): 250 (T5-2.13), 540 (4.7" readers), 480 / 800 (7.5" portrait / landscape) |
| 8 | 2 | page_h (u16): 122 (T5-2.13), 960 (4.7" readers), 800 / 480 (7.5") |
| 10 | 2 | stride (u16) = ceil(page_w / 8): 32, 68, 60 or 100 |
| 12 | 4 | page_count (u32) |
| 16 | 2 | toc_count (u16) |
| 18 | 64 | title |
| 82 | 64 | author |
| 146 | 52 × toc_count | TOC entries: start_page (u32, 0-based), title[48] |
| 146 + 52 × toc_count | page_bytes × page_count | pages |
| end of the pages | 12 + cover stride × cover h | optional cover block (below) |

`page_bytes = stride × page_h`: 3904 (T5-2.13), 65280 (4.7" readers) or 48000 (7.5",
either way). A reader only opens books whose page size matches its screen; the 7.5"
has two screens (Settings → Orientation) and lists books for either, opening those
for the one it shows. Page *n* (0-based) starts at

```
146 + 52 * toc_count + n * page_bytes
```

so the reader seeks straight to a page and needs no index.

## Page bitmap

Identical to bb_epaper's local framebuffer for the landscape 250×122 panel
(`setRotation(90|270)`), see `docs/hardware.md`, and to the portrait RAM canvas the
4.7" readers draw into before rotating it onto the panel (`docs/hardware-t5-47.md`):

- row-major, top row first, `stride` bytes per row
- MSB first: pixel *x* is bit `0x80 >> (x & 7)` of byte `x >> 3`
- **1 = white, 0 = black**
- the padding bits at the end of each row are 1 (6 on the T5-2.13, 4 on the 4.7" readers)

The bottom 14 rows (status bar) are white. The firmware draws the status bar
there after copying the page into the framebuffer.

## Cover block (optional)

Added in firmware 0.10.0 for the "cover" sleep screen. The version stays 1: a book
without a cover is unchanged. Older firmware checks the exact file size and refuses
a book with a cover, so those need 0.10.0 or later (`--no-cover`, or the page's
"Cover" box unticked, leaves it out). Right after the last page:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `"T5CV"` |
| 4 | 2 | w (u16) |
| 6 | 2 | h (u16) |
| 8 | 2 | stride (u16) = ceil(w / 8) |
| 10 | 2 | reserved = 0 |
| 12 | stride × h | bitmap, same layout as a page |

The cover is the EPUB's cover image scaled to fit 72×104 (T5-2.13), 420×640 (4.7"
readers), 340×520 (7.5" portrait) or 288×416 (7.5" landscape), sharpened and Atkinson dithered; the screen size gives the box (`cover_w`/`cover_h` in
`layout.py`). A file with anything else after the pages, or a block whose size
doesn't match, is invalid.

## Firmware sketch

```cpp
struct __attribute__((packed)) T5BHeader {
  char magic[4];        // "T5B1"
  uint16_t version, page_w, page_h, stride;
  uint32_t page_count;
  uint16_t toc_count;
  char title[64], author[64];
};                       // sizeof == 146
struct __attribute__((packed)) T5BTocEntry {
  uint32_t start_page;
  char title[48];
};                       // sizeof == 52
```

Validate magic, version, `page_w`/`page_h` against the compiled panel geometry and
`stride == (page_w + 7) / 8` before using a file. The current chapter for a page
is the last TOC entry with `start_page <= page`. In version 2 the page index follows
the TOC (`uint32_t index[page_count + 1]`).
