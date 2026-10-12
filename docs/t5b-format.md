# .t5b book format (version 1)

Written by `tools/bookprep` (`bookprep/t5b.py` is the reference implementation),
read by the firmware (Phase 3). Everything is little-endian. Strings are UTF-8,
NUL-padded and always NUL-terminated within their field.

## Layout

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `"T5B1"` |
| 4 | 2 | version (u16) = 1 |
| 6 | 2 | page_w (u16) = 250 |
| 8 | 2 | page_h (u16) = 122 |
| 10 | 2 | stride (u16) = ceil(page_w / 8) = 32 |
| 12 | 4 | page_count (u32) |
| 16 | 2 | toc_count (u16) |
| 18 | 64 | title |
| 82 | 64 | author |
| 146 | 52 × toc_count | TOC entries: start_page (u32, 0-based), title[48] |
| 146 + 52 × toc_count | page_bytes × page_count | pages |

`page_bytes = stride × page_h = 3904`. Page *n* (0-based) starts at

```
146 + 52 * toc_count + n * page_bytes
```

so the reader seeks straight to a page and needs no index.

## Page bitmap

Identical to bb_epaper's local framebuffer for the landscape 250×122 panel
(`setRotation(90|270)`), see `docs/hardware.md`:

- row-major, top row first, `stride` bytes per row
- MSB first: pixel *x* is bit `0x80 >> (x & 7)` of byte `x >> 3`
- **1 = white, 0 = black**
- the 6 padding bits at the end of each row are 1

The bottom 14 rows (status bar) are white. The firmware draws the status bar
there after copying the page into the framebuffer.

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
