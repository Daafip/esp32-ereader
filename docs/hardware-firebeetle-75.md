# FireBeetle 2 ESP32-C6 + Waveshare 7.5" (home-built)

A reader built from parts (Phase 10): a DFRobot FireBeetle 2 ESP32-C6, a Waveshare
7.5" e-Paper HAT B/W V2 on its e-Paper Driver HAT, three DFRobot Gravity buttons, a
microSD breakout and a Li-ion cell. Wiring, case and parts list for the original
(ESPHome) build: `epaper-display-wiring-case-specs.md`. Pins in the firmware:
`firmware/include/boards/firebeetle_75.h`; envs `reader_firebeetle75` (the reader) and
`fb75_test` (hardware test).

Status (2026-10-26): the hardware test ran on David's build (results below). The reader
itself has not run yet: `docs/checkpoints.md` → "FireBeetle 2 ESP32-C6 + 7.5"".

## Parts

| Part | Notes |
|---|---|
| DFRobot FireBeetle 2 ESP32-C6 (DFR1075) | 4 MB flash, no PSRAM, 512 KB SRAM, 16 KB RTC memory, one general-purpose SPI bus, Li-ion charger, battery divider on GPIO0. Deep sleep 36 µA (board v1.2) |
| Waveshare 7.5" e-Paper HAT B/W V2 | 800×480, UC81xx controller (bb_epaper `EP75_800x480_GEN2`), on the e-Paper Driver HAT **Rev2.3** (PWR pin) |
| 3 × DFRobot Gravity push button (DFR0029) | idle low (pull-down on the module), high when pressed |
| microSD breakout, 3.3 V | **not in the spec**: the reader keeps books on a card. Take one without a 5 V regulator or level shifter |
| Li-ion cell with protection | JST-PH 2.0 into the BAT socket |

Why the card: a 7.5" page is 48 KB (pages are stored as bitmaps, `docs/t5b-format.md`),
so a book is 10-20 MB. The 4 MB of flash holds the firmware twice (OTA) and no book.

## Wiring

HAT switches: Display Config **B** (0.47R), Interface Config **0** (4-wire SPI). HAT
PWR on 3V3: the panel is always powered.

| FireBeetle | Goes to | Notes |
|---|---|---|
| GPIO23 (SCK) | HAT CLK, card CLK | one SPI bus for both (the C6 has one) |
| GPIO22 (MOSI) | HAT DIN, card DI | |
| GPIO18 | HAT CS | |
| GPIO14 | HAT DC | |
| GPIO19 | HAT RST | GPIO20 gave no output on this board |
| GPIO21 (MISO) | HAT BUSY | low = busy (UC81xx) |
| GPIO6 (D12) | card DO (MISO) | added for the card |
| GPIO1 (D6) | card CS | added for the card |
| GPIO2 (A1) | button 1: PREV | LP GPIO: wakes from deep sleep |
| GPIO3 (A2) | button 2: NEXT | LP GPIO |
| GPIO7 (D11) | button 3: HOME (MENU) | LP GPIO |
| GPIO0 | battery divider (onboard) | × 2 |
| 3V3, GND | HAT VCC/PWR/GND, buttons, card | one shared bus |

Keep free: GPIO4, 5, 8, 9 (BOOT) and 15 (LED) are strapping pins, GPIO12/13 are USB,
GPIO16/17 UART0.

## How the firmware drives it

- **Panel:** `display_bbep.cpp` with bb_epaper's 7.5" V2 sequences: full refresh
  2.6 s, fast 1.5 s, partial 1.0 s (plus 0.23 s to send old and new image at 10 MHz).
  The copy of the shown image (48 KB) doesn't fit the C6's RTC memory, so it lives in
  RAM (`BOARD_SHOWN_IN_RAM`). After deep sleep the reader reads the page that is on
  the glass from the card again and sends it as the "old" image, then the new page
  with a partial refresh: the same as on the 4.7" boards. "Wake refresh: retain"
  behaves like "rewrite".
- **No panel power pin:** RST and CS are held high through deep sleep, so the
  controller stays in its deep sleep.
- **Shared SPI bus:** the bus starts with the card's MISO before bb_epaper (which would
  start it without), the card's CS is kept high while the panel talks, and every panel
  write sets the bus back to the panel's clock (the SD library leaves its own).
- **Buttons:** active high (`BOARD_BUTTONS_ACTIVE_HIGH`): NEXT short = next page,
  long = previous page; HOME = MENU (short: menu, long: the hold action); PREV =
  previous page or item. Deep sleep: any button wakes the chip (ext1 "any high"; the
  C6 has no ext0), and a PREV press that woke it turns back a page.
- **Memory:** 155 KB of static RAM (the two page buffers are 96 KB), app 1.67 MB of
  the 1.9 MB slot (`min_spiffs.csv`, as on the T5-2.13).

## Two screens: portrait and landscape

Settings → **Orientation**: portrait, portrait 180°, landscape, landscape 180° (the
other boards have "Flip screen" instead; NVS key `flip` either way).

| Screen id | Size | Default font / margin | Cover box |
|---|---|---|---|
| `7.5in` | 480×800 portrait | 18 px / 20 px | 340×520 |
| `7.5in-landscape` | 800×480 landscape | 22 px / 32 px | 288×416 |

A book is typeset for one of them (`pixi run prep --screen 7.5in` or
`--screen 7.5in-landscape`; the Wi-Fi page typesets for the orientation the reader is
in). The reader lists books for the other orientation too, marked "landscape" or
"portrait"; opening one says so. On the page they are marked as well, and
**Restyle** typesets them again for the current orientation. Both pages are exactly
48,000 bytes, so full-screen sleep pictures can't be told apart by size: portrait
ones live in `/sleep`, landscape ones in `/sleep/7.5in-landscape`.

## Checkpoint results

**2026-10-26, `fb75_test` on David's build** (no SD card fitted yet):

| Check | Result |
|---|---|
| Panel type | `EP75_800x480_GEN2`: full, fast and partial refresh work |
| Timing | full 2568 ms, fast 1495 ms, partial 986 ms (+ 228 ms data, both planes) |
| Orientation | rotation 90 (portrait) upright |
| BUSY | floated at first: no refresh at all, BUSY driven by nothing. A loose connection |
| Buttons | idle low on all three |
| Battery | 4230 mV (full cell), divider × 2 |
| bb_epaper buffer | identical to the `.t5b` page layout |

Not yet checked: the SD card, the button log, deep sleep (the test's first version had
a sleep step; it hung while BUSY was still broken, and was removed), sleep current.
