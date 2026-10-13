# .t5b book format (version 1)

Written by `tools/bookprep` (`bookprep/t5b.py` is the reference implementation),
read by the firmware (Phase 3). Everything is little-endian. Strings are UTF-8,
NUL-padded and always NUL-terminated within their field.

## Layout

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `"T5B1"` |
| 4 | 2 | version (u16) = 1 |
| 6 | 2 | page_w (u16): 250 (T5-2.13) or 540 (4.7" readers) |
| 8 | 2 | page_h (u16): 122 (T5-2.13) or 960 (4.7" readers) |
| 10 | 2 | stride (u16) = ceil(page_w / 8): 32 or 68 |
| 12 | 4 | page_count (u32) |
| 16 | 2 | toc_count (u16) |
| 18 | 64 | title |
| 82 | 64 | author |
| 146 | 52 × toc_count | TOC entries: start_page (u32, 0-based), title[48] |
| 146 + 52 × toc_count | page_bytes × page_count | pages |
| end of the pages | 12 + cover stride × cover h | optional cover block (below) |

`page_bytes = stride × page_h`: 3904 (T5-2.13) or 65280 (4.7" readers). A reader only
opens books whose page size matches its screen. Page *n* (0-based) starts at

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

The cover is the EPUB's cover image scaled to fit 72×104 (T5-2.13) or 420×640 (4.7"
readers), sharpened and Atkinson dithered; the screen size gives the box (`cover_w`/`cover_h` in
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
is the last TOC entry with `start_page <= page`.
