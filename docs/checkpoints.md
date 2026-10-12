# Hardware checkpoints: what to run when the board arrives

Everything up to Phase 5 is written and builds; nothing has run on a board yet.
Work top to bottom. Each step says what to send back; results go into
`docs/hardware.md` → "Checkpoint results".

General:

- **Remove the SD card before flashing** (GPIO2 strapping pin). If an upload
  won't start: hold BOOT, tap RST, release BOOT.
- Serial output: `pixi run fw-monitor` (115200 baud). Copy-paste the relevant lines.

## Checkpoint 0: identify the hardware

1. Read the label on the display's ribbon cable (e.g. `DEPG0213BN`, `GDEM0213B74`) and
   the board version printed on the PCB (V2.3.1 / V2.4).
2. `pixi run fw-flash panel_tester`, then `pixi run fw-monitor`.
   - It prints `CONTROLLER-ID: SSD16xx` (or UC81xx) and a candidate list.
   - Each candidate draws a test screen: full refresh, 5 partial updates (counter
     bottom right), a fast refresh. Press USER (GPIO39) for the next candidate,
     BOOT to repeat, or type `n`, `r`, `0`-`9`, `c` (clear) in the monitor.
   - **Send:** the CONTROLLER-ID line, all `RESULT ...` lines, and which candidate
     looks right (sharp text, complete border, no stripes or noise).
3. `pixi run fw-flash button_test`: press each button short, long and rapidly.
   **Send:** the log (idle levels, press/release lines). Any "extra(..)" lines mean a
   4-button variant.
4. `pixi run fw-flash sd_test` with the card out, then insert a FAT32 card and press
   RST. **Send:** the `BUS:`, `MOUNT:`, `CARD:`, `4K:`, `RANDOM READ` and `RESULT` lines,
   and whether the board boots normally with the card in.

I then set the panel constant and pins in `firmware/include/boards/t5_213.h`.

## Checkpoint 1: calibration and ghosting

`pixi run fw-flash calibration`, monitor open.

1. Check the calibration page: border complete on all four sides with ticks, corners
   TL filled square / TR hollow square / BL triangle / BR cross, arrow pointing at
   the edge you want as "top", staircase `\\\\\\\\` (not `////`).
   - Arrow points down? Hold NEXT (long press) to flip rotation 90 ↔ 270.
   - **Send:** photo of the screen, the rotation that is upright, timing lines.
2. Short press NEXT: ghosting test, 30 partial updates alternating two text pages,
   step counter bottom right, then a 10 s pause and one full refresh.
   **Send:** after how many partial updates the ghosting bothers you (that becomes the
   default "Full refresh: every N"), and the "partial refresh average" line.
3. Short press MENU: fast refresh timing. **Send:** the timing line.

## Checkpoint 2: font (no board needed) — done

Decided 2026-10-12: **Ubuntu Regular 13 px**, threshold rendering, default margins.
These are bookprep's defaults, so `pixi run prep book.epub` needs no font options.
The rest of this section is for revisiting the choice later.

Look at `out/checkpoint2/font-comparison.png` and `font-comparison-mono.png`, or make
your own with any fonts and book:

```bash
pixi run font-sheet book.epub --font a.ttf --font b.ttf --sizes 11 12 13 --mono --out out/sheet.png
```

**Send:** font, size, threshold vs mono, and margin wishes. Then convert a book:

```bash
pixi run prep path/to/book.epub --font path/to/font.ttf --size 12 --mono --preview
```

Copy `out/<book>.t5b` to `/books/` on the SD card. `out/devpages.t5b` (calibration
page + 2 text pages) is a quick test book.

## Checkpoint 3: reading

`pixi run fw-flash reader`, card with `/books/*.t5b` inserted after flashing.
For this first test set Menu → Settings → "Sleep after page: off" so the serial
monitor keeps working; it logs every page with refresh type and timing.

**Send:** latency feel, ghosting, legibility of text and status bar, anything odd in
the menus (Chapters, Go to %, Library, Settings, About).

## Wi-Fi transfer (after Checkpoint 3)

1. Menu → Wi-Fi transfer. Scan the first QR code with a phone: it should join
   `T5-Reader-XXXX`. The screen should switch to the second QR code by itself.
2. Scan it (or open `http://192.168.4.1` and type the code). Upload a `.t5b`, try a
   non-book file (must be refused), delete a book. Watch the e-ink event line.
3. From the phone: drop an EPUB, try a few fonts and sizes in the preview, "Upload to
   reader". Open it on the reader. Then "Restyle" it with another size: the reading
   position should land in about the same place. Add a font of your own.
3. Optional: "Update firmware" with `t5-reader-<version>-ota.bin` from
   `pixi run fw-dist`; also try the merged `t5-reader-<version>.bin` (must be refused
   with "Magic byte is wrong").
4. MENU on the reader: back to the library, new books listed.

**Send:** anything that didn't work, upload speed for a big book (serial log shows
sizes), and whether the phone complained about "no internet".

## Checkpoint 4: power

Set "Sleep after page: on" (default). Each page turn is now a wake from deep sleep.

1. For each Settings → "Wake refresh" value (`rewrite`, `retain`, `full`): turn ~15
   pages forward and back. **Send:** which give clean partial updates, the
   `page ... at N ms` lines (time from wake to image, excluding ~0.2-0.3 s boot), and
   whether `retain` shows garbage (the controller lost its RAM).
2. Sleep current: measure in series with the battery (USB unplugged) while a page is
   shown, for `rewrite` and `retain`. LilyGo measured ~30 µA on V2.4.
3. Long-press feel: is 600 ms right? (Settings → Long press.)
4. Wake via BOOT while a page is shown must open the menu, not download mode.
5. Idle: leave it 10 min after a page turn; the sleep screen should appear.

## T5-4.7" S3 (second board, if you get one)

No diagnostic sketches for this board: the reader itself is the test. Results go in
`docs/hardware-t5-47.md` → "Checkpoint results".

1. `pixi run fw-flash reader_t547` (no port? hold BOOT, press RST, release BOOT),
   then `pixi run fw-monitor`. Card with `/books/*.t5b` made for this screen:
   `pixi run prep book.epub --screen t5-47`, or Wi-Fi transfer (the page typesets
   for the screen the reader reports).
2. First screen: is it upright with the USB port where you want "down", and not
   mirrored? If not, change `kEpdRotation` (90/270) in `boards/t5_47_s3.h`, or tell
   me about the mirroring. **Send:** a photo.
3. Settings → "Sleep after page: off". Turn pages by tapping: the serial log shows
   `page ... full/partial refresh, data N ms, refresh N ms`. **Send:** the lines,
   ghosting after how many partial turns, and how tap / double tap / hold / long hold feel.
4. "Sleep after page: on": each tap wakes the chip. **Send:** the `page ... at N ms`
   lines (wake to image) and whether the first turn after waking is partial and clean
   (the reader rebuilds the old page from the card for that).
5. Leave it asleep with a page shown and measure the current in series with the
   battery (LilyGo: ~380 µA). About → Battery should read about the cell voltage.
