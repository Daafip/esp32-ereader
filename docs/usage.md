# Using the reader

## Books

1. Convert on the PC: `pixi run prep book.epub --out out/ --preview`
   (`--font`, `--size`, `--mono`, `--lang` etc.: `pixi run prep --help`).
2. Copy `out/<book>.t5b` to `/books/` on a FAT32 SD card.
3. Insert the card (after flashing; uploading fails with the card in).

The reader reopens the last book at the last page. Each book remembers its own page.

## Buttons

| | NEXT (GPIO39) | MENU (BOOT, GPIO0) |
|---|---|---|
| Reading, short | next page | menu |
| Reading, long (≥ 600 ms) | previous page | sleep screen |
| Lists, short | next item | select / change |
| Lists, long | previous item (repeats while held) | back |
| Go to %, short / long | +1 % / +10 % | go / back |

## Menu

- **Resume**: back to the page.
- **Chapters**: jump to a chapter (• marks the current one).
- **Go to %**: jump to a position in the book.
- **Library**: all books in `/books`, author and page count at the bottom.
- **Settings** (select changes the value):
  - *Full refresh*: a full (flashing) refresh after this many page turns to clear ghosting.
    Chapter changes always get one.
  - *Long press*: threshold between short and long press.
  - *Idle sleep*: after this long without a button, the sleep screen appears.
  - *Sleep after page*: deep sleep between page turns (battery life). Turn off for
    debugging over USB.
  - *Wake refresh*: how a page turn after deep sleep refreshes (`rewrite`, `retain`,
    `full`; see `docs/hardware.md`).
- **About**: firmware, battery, book, position, card, wake timing.
- **Sleep**: show the sleep screen now.

The status bar shows the chapter, page/total, percentage and battery.
Below 3.4 V the reader shows "Battery low" and stays asleep until charged.
