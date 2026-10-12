# Using the reader

## Books

1. Convert on the PC: `pixi run prep book.epub --out out/ --preview`
   (`--font`, `--size`, `--mono`, `--lang` etc.: `pixi run prep --help`).
2. Copy `out/<book>.t5b` to `/books/` on a FAT32 SD card.
3. Insert the card (after flashing; uploading fails with the card in).

The reader reopens the last book at the last page. Each book remembers its own page.

## Wi-Fi transfer (books from a browser)

Menu → **Wi-Fi transfer** (or MENU on the "No books yet" screen).

1. **Join the Wi-Fi**: scan the QR code with your phone, or join `T5-Reader-XXXX`
   with the password on the screen (it stays the same, so laptops can remember it).
2. **Open the page**: as soon as a device joins, the screen shows a second QR code
   with the page address and a 4-digit code. Typing `http://192.168.4.1` works too;
   the page then asks for the code.
3. Drop `.t5b` files on the page (or tap to choose). It lists the books on the card
   with how far you've read each, and can delete them.
4. Press MENU on the reader when done. Wi-Fi turns off; it also turns off after the
   idle time without uploads or button presses.

NEXT switches between the two QR codes. Uploads are checked: a file that isn't a
valid `.t5b` for this screen is refused and not stored.

**Home network instead:** copy `firmware/include/secrets.h.example` to `secrets.h`,
fill in your Wi-Fi name and password and flash again. The reader then joins your
network (falls back to its own if that fails) and shows its address and code.

**Firmware updates over Wi-Fi:** the same page has "Update firmware". Upload
`out/firmware/t5-reader-<version>-ota.bin` from `pixi run fw-dist`; the reader checks
the image, restarts into it and leaves Wi-Fi mode. If anything goes wrong, flash over
USB as below.

## Flashing

- **From this PC:** `pixi run fw-flash reader` (SD card out while flashing).
- **From a browser, no tools needed:** `pixi run fw-dist` puts one image per sketch
  in `out/firmware/` (`t5-reader-<version>.bin`, `t5-calibration-...`, etc.). In
  Chrome or Edge open <https://espressif.github.io/esptool-js/>, Connect (pick the
  USB serial port), Flash Address `0x0`, choose the `.bin`, Program. Use the files
  *without* `-ota` here: they contain bootloader, partition table and app.
- **Over Wi-Fi:** see "Firmware updates over Wi-Fi" above (after the first flash).

## Buttons

The footer of each menu shows the keys: a black chip is the button (NEXT or MENU),
an outlined "hold" chip is the long press.

| | NEXT (GPIO39) | MENU (BOOT, GPIO0) |
|---|---|---|
| Reading, short | next page | menu |
| Reading, long (≥ 600 ms) | previous page | sleep screen |
| Lists, short | next item | select / change |
| Lists, long | previous item (repeats while held) | back |
| Go to position, short / long | +1 % / +10 % | go / back |

## Menu

- **Resume**: back to the page.
- **Chapters**: jump to a chapter; start page on the right, a dot marks the current one.
- **Go to position**: jump to a percentage of the book.
- **Library**: all books in `/books` with how far you've read each ("new" if unopened).
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

The status bar shows a reading-progress line, the chapter, page/total, percentage
and battery.
Below 3.4 V the reader shows "Battery low" and stays asleep until charged.
