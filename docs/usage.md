# Using the reader

Four boards: the LilyGo **T5-2.13** (the original), the LilyGo **T5-4.7" S3**, and
M5Stack's **M5PaperS3** and **M5Paper** (later build targets, untested on hardware; see
`docs/hardware-t5-47.md` and `docs/hardware-m5paper.md`). The last three share a 4.7"
960×540 screen. Everything below applies to all of them unless it says otherwise.

## Books

**From your phone (no laptop):** start Wi-Fi transfer on the reader (below), open the
page and drop an `.epub` or `.txt`. A sheet opens with a pixel-exact preview of the
reader's screen and the typesetting options:

- **Font:** Ubuntu (default), Literata, Atkinson Hyperlegible, Merriweather, system
  fonts, or "Add a font…" with any `.ttf`/`.otf`/`.woff2` (stored on the reader's card in
  `/fonts`, so it's there next time).
- **Size, margins, line spacing, weight** (lighter/bolder), **justify**, **indent**,
  **chapters on a new page**.
- **Hyphenation:** Dutch, English (UK/US), French, or off. "Auto" follows the book's
  language. German books are typeset without hyphenation (its patterns are too big for
  the reader's flash).

"Upload to reader" typesets the book in the browser and sends only the `.t5b`. The reader
also keeps the original and your settings, so **Restyle** in the book list lets you
change the font or size later from any phone; your reading position moves along
proportionally. "Download .t5b" saves the result instead (e.g. for the SD card).

The page asks the reader for its screen size and typesets for it: 250×122 at 13 px
on the T5-2.13, 540×960 at 28 px on the 4.7" readers. A `.t5b` only opens on the
screen it was made for (books for one 4.7" reader open on the others).

Large books take a moment: Max Havelaar is 3600 pages, ~14 MB to send to the T5-2.13
(about 55 MB to the T5-4.7", whose pages are 17× bigger). Keep the phone's screen on
while it uploads.

**On the PC:** `pixi run prep book.epub --out out/ --preview` (`--font`, `--size`, `--mono`,
`--lang`, ...: `pixi run prep --help`; add `--screen 4.7in` for the 4.7" readers), then
copy `out/<book>.t5b` to `/books/` on the card (FAT32). Page breaks differ slightly from the browser: the two use different font
engines.

**Try the page without the reader:** `pixi run web-dev --lan` runs it on your PC with a
folder as fake SD card (`out/web-dev/sd`) and prints an address for your phone
(`--screen 4.7in` to pretend to be a 4.7" reader).

The reader reopens the last book at the last page. Each book remembers its own page.

## Wi-Fi transfer (books from a browser)

Menu → **Wi-Fi transfer** (or MENU on the "No books yet" screen).

1. **Join the Wi-Fi**: scan the QR code with your phone, or join `T5-Reader-XXXX`
   with the password on the screen (it stays the same, so laptops can remember it).
2. **Open the page**: as soon as a device joins, the screen shows a second QR code
   with the page address and a 4-digit code. Typing `http://192.168.4.1` works too;
   the page then asks for the code.
3. Drop `.epub`/`.txt` files (typeset in the browser, see "Books") or ready `.t5b`
   files on the page. It lists the books on the card with how far you've read each,
   and can restyle or delete them.
4. Press MENU on the reader when done (T5-4.7": hold the button; touch: "done").
   Wi-Fi turns off; it also turns off after the idle time without uploads or button
   presses.

NEXT (T5-4.7": tap; touch: the left button) switches between the two QR codes. Uploads are checked: a file that isn't a
valid `.t5b` for this screen is refused and not stored.

**Home network instead:** copy `firmware/include/secrets.h.example` to `secrets.h`,
fill in your Wi-Fi name and password and flash again. The reader then joins your
network (falls back to its own if that fails) and shows its address and code.

**Firmware updates over Wi-Fi:** the same page has "Update firmware". Upload
the `-ota.bin` for your board from a release or `pixi run fw-dist`: `t5-reader-…`,
`t5-47-reader-…`, `m5papers3-reader-…` or `m5paper-reader-…` (the page names the
right one; another board's file is refused). The reader checks the image, restarts
into it and leaves Wi-Fi mode. If anything goes
wrong, flash over USB as below.

## Flashing

- **From this PC:** `pixi run fw-flash reader` (SD card out while flashing).
  T5-4.7": `pixi run fw-flash reader_t547` (if no port shows up: hold BOOT, press
  RST, release BOOT). M5PaperS3: `reader_m5papers3`, M5Paper: `reader_m5paper`.
- **Ready-made images:** every release on
  <https://github.com/Daafip/esp32-ereader/releases> has the images below attached
  (built by GitHub Actions from the version tag).
- **From a browser, no tools needed:** `pixi run fw-dist` puts one image per sketch
  in `out/firmware/` (`t5-reader-<version>.bin`, `t5-calibration-...`, etc.). In
  Chrome or Edge open <https://espressif.github.io/esptool-js/>, Connect (pick the
  USB serial port), Flash Address `0x0`, choose the `.bin`, Program. Use the files
  *without* `-ota` here: they contain bootloader, partition table and app.
  The other boards: `t5-47-reader-…`, `m5papers3-reader-…`, `m5paper-reader-…`,
  also at `0x0`.
- **Over Wi-Fi:** see "Firmware updates over Wi-Fi" above (after the first flash).

## Buttons

The footer of each menu shows the keys: a black chip is the button (NEXT or MENU),
an outlined "hold" chip is the long press. On the one-button T5-4.7" the chips name
the gestures instead: TAP and "2×", HOLD and "long".

### T5-2.13: two buttons

| | NEXT (GPIO39) | MENU (BOOT, GPIO0) |
|---|---|---|
| Reading, short | next page | menu |
| Reading, long (≥ 600 ms) | previous page | sleep screen |
| Lists, short | next item | select / change |
| Lists, long | previous item (repeats while held) | back |
| Go to position, short / long | +1 % / +10 % | go / back |

### T5-4.7": one button (GPIO21)

| | tap | double tap | hold (≥ 600 ms) | long hold (≥ 1.6 s) |
|---|---|---|---|---|
| Reading | next page | previous page | menu | sleep screen |
| Lists | next item | previous item | select / change | back |
| Go to position | +1 % | +10 % | go | back |

A tap waits 300 ms for a possible second tap, so a page turn starts that much later.
Hold time follows Settings → Long press (long hold = that + 1 s). A hold fires when
you let go; a long hold fires while you keep holding. The BOOT button is wired to
the display, so the reader can't use it.

### M5PaperS3 and M5Paper: touch

| | Reading | Lists (menu, chapters, library, settings) |
|---|---|---|
| Tap | right two thirds: next page; left third: previous; top strip: menu | a row: open / select / change it |
| Swipe left / right | next / previous page | |
| Swipe up / down | | a screen of rows down / up |
| Long press (≥ 600 ms) | menu | back |
| Footer buttons | | as labelled (▼ ▲, open, back, ...) |

Go to position: tap the bar to jump straight there, or use +1 / +10. On messages a tap
anywhere counts as "any button".

The M5Paper's 3-way switch works as well: down = NEXT, press = MENU, up = previous
(page or item). The M5PaperS3 has no buttons besides power.

**Sleep on the M5 boards:** between page turns they light-sleep (a touch or the switch
wakes them, the page stays). When idle, or Menu → Sleep, they show the sleep screen and
switch off; the power button (M5Paper: the switch) turns them on at the same page.

## Menu

- **Resume**: back to the page.
- **Chapters**: jump to a chapter; start page on the right, a dot marks the current one.
- **Go to position**: jump to a percentage of the book.
- **Library**: all books in `/books` with how far you've read each ("new" if unopened).
- **Settings** (select changes the value; on the T5-4.7" "retain" behaves like
  "rewrite": the panel keeps no image, so the reader reads the shown page back from
  the card):
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

The status bar shows a reading-progress line, the chapter, page/total, percentage
and battery.
Below 3.4 V the reader shows "Battery low" and stays asleep until charged.
