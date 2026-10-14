# Using the reader

Four boards: the LilyGo **T5-2.13** (the original, tested on a V2.3.1 board), the
LilyGo **T5-4.7" S3**, and M5Stack's **M5PaperS3** and **M5Paper** (later build
targets, untested on hardware; see
`docs/hardware-t5-47.md` and `docs/hardware-m5paper.md`). The last three share a 4.7"
960×540 screen. Everything below applies to all of them unless it says otherwise.

## Installing the firmware

**From a browser (recommended):** open <https://daafip.github.io/esp32-ereader/> in
Chrome or Edge on a computer, plug the reader in over USB, pick your board and click
Install. Leave **"Erase everything"** unticked: then your settings, bookmarks,
reading positions and statistics stay. Books live on the SD card and are
never touched. T5-2.13: take the SD card out while installing. No port in the list?
Hold BOOT, press RST, release BOOT and try again.
On Linux with Chromium from the Snap Store, run `sudo snap connect chromium:raw-usb`
once and restart Chromium (otherwise it can't open the USB port), or use Google
Chrome. Firefox has no Web Serial, so it can't flash.

The installer page is built by `pixi run installer` (→ `out/installer/`) and published
by the release workflow.

Other ways:

- **From this PC:** `pixi run fw-flash reader` (SD card out while flashing).
  T5-4.7": `pixi run fw-flash reader_t547` (if no port shows up: hold BOOT, press
  RST, release BOOT). M5PaperS3: `reader_m5papers3`, M5Paper: `reader_m5paper`.
  This keeps settings too.
- **Ready-made images:** every release on
  <https://github.com/Daafip/esp32-ereader/releases> has the images attached (built
  by GitHub Actions from the version tag).
- **esptool-js:** <https://espressif.github.io/esptool-js/>, Flash Address `0x0`, the
  `.bin` *without* `-ota` (`t5-reader-…`, `t5-47-reader-…`, `m5papers3-reader-…`,
  `m5paper-reader-…`). This image contains bootloader, partition table and app and
  **erases the settings** (books on the card stay).
- **Over Wi-Fi:** see "Firmware updates over Wi-Fi" below (after the first install).
  Keeps everything.

## Books

**From your phone (no laptop):** start Wi-Fi transfer on the reader (below), open the
page and drop an `.epub` or `.txt`. A sheet opens with a pixel-exact preview of the
reader's screen and the typesetting options:

- **Font:** Ubuntu (default), Literata, Atkinson Hyperlegible, Merriweather,
  OpenDyslexic, system fonts, or "Add a font…" with any `.ttf`/`.otf`/`.woff2` (stored
  on the reader's card in `/fonts`, so it's there next time).
- **Size, margins, line spacing, weight** (lighter/bolder), **justify**, **indent**,
  **chapters on a new page**.
- **Bionic reading:** the first part of each word in bold, as a guide for the eye.
- **Cover:** keep the book's cover picture (for the "cover" sleep screen).
- **Folder:** put the book in a folder of the library (or make a new one).
- **Hyphenation:** Dutch, English (UK/US), French, or off. "Auto" follows the book's
  language. German books are typeset without hyphenation (its patterns are too big for
  the reader's flash).

"Upload to reader" typesets the book in the browser and sends only the `.t5b`. The reader
also keeps the original and your settings, so **Restyle** in the book list lets you
change the font or size later from any phone; your reading position and bookmarks move
along proportionally. "Download .t5b" saves the result instead (e.g. for the SD card).

The page asks the reader for its screen size and typesets for it: 250×122 at 13 px
on the T5-2.13, 540×960 at 28 px on the 4.7" readers. A `.t5b` only opens on the
screen it was made for (books for one 4.7" reader open on the others).

Large books take a moment: Max Havelaar is 3600 pages, ~14 MB to send to the T5-2.13
(about 55 MB to the T5-4.7", whose pages are 17× bigger). Keep the phone's screen on
while it uploads.

**On the PC:** `pixi run prep book.epub --out out/ --preview` (`--font`, `--size`, `--mono`,
`--lang`, `--bionic`, `--no-cover`, ...: `pixi run prep --help`; add `--screen 4.7in` for
the 4.7" readers), then copy `out/<book>.t5b` to `/books/` (or a folder in it) on the
card (FAT32). Page breaks differ slightly from the browser: the two use different font
engines.

**Try the page without the reader:** `pixi run web-dev --lan` runs it on your PC with a
folder as fake SD card (`out/web-dev/sd`) and prints an address for your phone
(`--screen 4.7in` to pretend to be a 4.7" reader).

The reader reopens the last book at the last page. Each book remembers its own page.

### Folders

The library shows the books in `/books` and one level of folders below it (a folder
row shows how many books it holds; select it to open, "back" goes up). Make folders
from the page: the Folder field when uploading, or **Move** in the book list. On the
card a folder is just `/books/<name>/`.

### Reading a book on the page, searching

**Open** in the book list shows the book's pages as the reader has them, with its
bookmarks (tap one to go there) and "Bookmark this page". **Search in the book** finds
words in books added from the page (it needs the original the reader keeps) and lists
the pages with a snippet. "Read from here on the reader" sets the reading position; the
reader opens that page when you press done on it.

## Wi-Fi transfer (books from a browser)

Menu → **Wi-Fi transfer**, or any press on the "No books yet" screen.

1. **Join the Wi-Fi**: scan the QR code with your phone, or join `T5-Reader-XXXX`
   with the password on the screen (it stays the same, so laptops can remember it).
2. **Open the page**: as soon as a device joins, the screen shows a second QR code
   with the page address and a 4-digit code. Typing `http://192.168.4.1` works too;
   the page then asks for the code.
3. Drop `.epub`/`.txt` files (typeset in the browser, see "Books") or ready `.t5b`
   files on the page. It lists the books on the card with how far you've read each,
   and can open, restyle, move or delete them.
4. Hold the button on the reader when done (touch: "done"; M5Paper switch: press).
   Wi-Fi turns off; it also turns off after the idle time without uploads or button
   presses.

A tap (touch: the left button) switches between the two QR codes. Uploads are checked: a file that isn't a
valid `.t5b` for this screen is refused and not stored.

The page also shows your reading statistics, and opening it sets the reader's clock
(the reader has no clock of its own; statistics per day need it).

**Home network instead:** on the page, ⚙ Settings → Home Wi-Fi: network name and
password. The next time you start Wi-Fi transfer the reader joins that network (and
makes its own if it can't) and shows its address and code. The password stays on the
reader; the page never shows it again. "Forget" goes back to the reader's own network.
(Developers can still put a network in `firmware/include/secrets.h`; the one from the
page comes first.)

**Settings from the page:** ⚙ Settings has the reader's settings (the same as on the
reader, below) and a **Name** for the reader, shown above its library.

**Firmware updates over Wi-Fi:** the same page has "Update firmware". Upload
the `-ota.bin` for your board from a release or `pixi run fw-dist`: `t5-reader-…`,
`t5-47-reader-…`, `m5papers3-reader-…` or `m5paper-reader-…` (the page names the
right one; another board's file is refused). The reader checks the image, restarts
into it and leaves Wi-Fi mode. If anything goes wrong, install over USB as above.

**Factory reset:** ⚙ Settings → Factory reset → "Erase everything", then type ERASE.
It deletes all books, originals, bookmarks, pictures and fonts from the card, and all
settings, positions, statistics and the home Wi-Fi from the reader, then restarts.

## Buttons

The footer of each menu shows the keys. On the T5 boards the chips name the
gestures: TAP and "2×", HOLD and "long". (The firmware also knows a two-button
layout, NEXT and MENU, with an outlined "hold" chip for the long press; no supported
board uses it at the moment.)

### T5-2.13 and T5-4.7": one button

The T5-2.13 uses its user button (GPIO39); the other button, RST, restarts the
reader. Only the V2.4 board has a BOOT button as well, so the reader doesn't use
it. The T5-4.7" uses its user button (GPIO21).

| | tap | double tap | hold (≥ 600 ms) | long hold (≥ 1.6 s) |
|---|---|---|---|---|
| Reading | next page | previous page | menu | Settings → *Long hold* (default: sleep screen) |
| Lists | next item | previous item | select / change | back |
| Go to position | +1 % | +10 % | go | back |

A tap waits 300 ms for a possible second tap, so a page turn starts that much later.
Hold time follows Settings → Hold time (long hold = that + 1 s). A hold fires when
you let go; a long hold fires while you keep holding. On the T5-4.7" the BOOT button
is wired to the display, so the reader can't use it.

### M5PaperS3 and M5Paper: touch

| | Reading | Lists (menu, chapters, library, settings) |
|---|---|---|
| Tap | right two thirds: next page; left third: previous; top strip: menu | a row: open / select / change it |
| Swipe left / right | next / previous page | |
| Swipe up / down | | a screen of rows down / up |
| Long press (≥ 600 ms) | Settings → *Long press* (default: menu) | back |
| Footer buttons | | as labelled (▼ ▲, open, back, ...) |

Go to position: tap the bar to jump straight there, or use +1 / +10. On messages a tap
anywhere counts as "any button".

The M5Paper's 3-way switch works as well: down = NEXT, press = MENU, up = previous
(page or item). The M5PaperS3 has no buttons besides power.

**Sleep on the M5 boards:** between page turns they light-sleep (a touch or the switch
wakes them, the page stays). When idle, or Menu → Sleep, they show the sleep screen and
switch off; the power button (M5Paper: the switch) turns them on at the same page.

### The hold gesture

A long hold (T5 boards; M5Paper switch: hold the press) or long press (touch) while
reading does what Settings → *Long hold* / *Long press* says: **sleep** (sleep screen),
**bookmark** (add or remove a bookmark on this page), **lock**, or **menu**.

## Menu

- **Resume**: back to the page.
- **Chapters**: jump to a chapter; start page on the right, a dot marks the current one.
- **Bookmarks**: the first row adds or removes a bookmark on the current page, the
  others jump to a bookmark (chapter, page on the right). A ribbon in the status bar
  marks a bookmarked page. Up to 100 per book, kept on the card in
  `/books/.marks/<book>.txt` (one page number per line).
- **Go to position**: jump to a percentage of the book.
- **Library**: the books in `/books` with how far you've read each ("new" if
  unopened), and its folders.
- **Statistics**: pages and minutes read today, the streak of days in a row with
  reading (and the best one), days read and the total. A page counts at most
  3 minutes, so a reader left open doesn't add up. Days need the clock, which the
  Wi-Fi page sets; the T5 boards lose it when the battery runs empty or on a reset
  (then open the page once), the M5 boards keep it in their clock chip.
- **Wi-Fi transfer**: see above.
- **Settings** (select changes the value; the page's ⚙ Settings has the same list):
  - *Sleep screen*: what shows while the reader sleeps: **book** (title, author,
    progress), **cover** (the same with the book's cover; books without one fall back
    to "book"), **pictures** (your own pictures in turn, made on the page under "Sleep
    pictures") or **keep page** (the page stays; the next press turns it).
    The picture editor shows exactly what the reader will show. *Method*: Atkinson
    (default: clean whites and blacks), Floyd–Steinberg (more detail, more noise),
    pattern, or threshold (drawings, text). *Brightness* and *contrast* help with
    dark photos (e-paper looks darker than a screen); *sharpen* keeps small details.
  - *Status bar*: **full** (progress line, chapter, page/total, percentage, battery),
    **minimal** (progress line and percentage) or **hidden**.
  - *Long hold* (touch: *Long press*): see "The hold gesture".
  - *Lock when asleep*: lock the reader every time the sleep screen appears.
  - *Language*: English or Nederlands (menus, messages and the page).
  - *Full refresh*: a full (flashing) refresh after this many page turns to clear
    ghosting. Chapter changes always get one.
  - *Hold time*: threshold between short and long press.
  - *Idle sleep*: after this long without a button, the sleep screen appears.
  - *Sleep after page*: deep sleep between page turns (battery life). Turn off for
    debugging over USB.
  - *Wake refresh*: how a page turn after deep sleep refreshes (`rewrite`, `retain`,
    `full`; see `docs/hardware.md`). On the T5-2.13 and the T5-4.7" "retain" behaves
    like "rewrite": every partial update sends the previous page again.
- **About**: firmware, battery, book, position, card, wake timing.
- **Lock**: lock the reader (below).
- **Sleep**: show the sleep screen now.

Below 3.4 V the reader shows "Battery low" and stays asleep until charged.

### Lock

Locked, the reader ignores every press (in a bag, in a pocket) except the unlock
gesture: a long hold (T5 boards) or long press (touch). The status bar
says "Locked" with that gesture and shows a padlock, so does the sleep screen. Lock
from the menu, with the hold gesture (Settings → *Long hold*: lock), or every time
it goes to sleep (Settings → *Lock when asleep*). Lock is not a security feature:
anyone who knows the gesture can unlock it, and the card can be taken out.
