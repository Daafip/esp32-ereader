# Hardware notes: LilyGo T5-4.7" S3 (second board)

The reader also builds for the LilyGo T5-4.7" S3 (T5-ePaper-S3). It started as an
example of what porting to another board takes. Nothing here has run on a board yet:
**LILYGO** = from LilyGo's repo, **VERIFY** = to check on a board.

Source: <https://github.com/Xinyuan-LilyGO/LilyGo-EPD47> (commit `5022422`, 2026-09):
README GPIO list, `src/ed047tc1.h`, `src/utilities.h`, `boards/T5-ePaper-S3.json`.
Not covered: the older ESP32 (WROVER) T5-4.7", the T5 E-Paper S3 Pro (another board
with a TPS65185 and an I/O expander), and the optional touch panel.

## Board

| Item | Value | Status |
|---|---|---|
| MCU | ESP32-S3R8: 16 MB flash, 8 MB OPI PSRAM | LILYGO |
| USB | native USB (serial over USB CDC) | LILYGO |
| Panel | ED047TC1, 4.7", 960×540, 16 grey levels, parallel (no controller, no RAM) | LILYGO |
| Control lines | 74HCT4094 shift register: output enable, mode, STV, ± rails, power disable, latch | LILYGO |
| Sleep current | ~380 µA (LilyGo, button wake) | LILYGO |

## Pins

| Function | GPIO | Notes | Status |
|---|---|---|---|
| Panel D0..D7 | 8, 1, 2, 3, 4, 5, 6, 7 | ESP32-S3 LCD peripheral (i80) | LILYGO |
| CKV | 38 | | LILYGO |
| STH (SPH, start pulse) | 40 | | LILYGO |
| CKH (CL, pixel clock) | 41 | | LILYGO |
| 74HCT4094 data / clock / strobe | 13 / 12 / 0 | strobe = BOOT button | LILYGO |
| Dummy D/C for the LCD driver | 48 | README: "No Connect any" | LILYGO |
| Button | 21 | RTC GPIO (ext0 wake); internal pull-up on | LILYGO / VERIFY pull-up |
| Battery ADC | 14 | ADC2_CH3, divider ×2 | LILYGO |
| SD SCK / MISO / MOSI / CS | 11 / 16 / 15 / 42 | | LILYGO |
| RTC + touch I2C SDA / SCL, touch IRQ | 18 / 17, 47 | not used | LILYGO |
| Free | 45, 10, 48, 39 | 45 is a strapping pin | LILYGO |

All in `firmware/include/boards/t5_47_s3.h`.

## Display library

- **FastEPD 2.2.0** (`bitbank2/FastEPD@2.2.0`, Apache-2.0, same author as bb_epaper)
  drives the panel. It has no preset for this board. Its LilyGo T5 Pro preset uses the
  same 74HCT4094 control scheme on other pins, so `src/fastepd_panel.cpp` defines a
  custom panel with this board's pins and reuses FastEPD's `LilyGoEinkPower`,
  `LilyGoIOInit` and `LilyGoRowControl`.
  - The shift-register bit order matches LilyGo's own driver (OE, mode, scan direction,
    STV, −, +, power disable, latch = bits 7..0).
  - `FASTEPD::initCustomPanel()` in 2.2.0 only runs the I/O callback. The subclass does
    what `bbepInitPanel()` does: `bbepIOInit()` (bus) + `bbepSetPanelSize()` (buffers
    in PSRAM).
- **bb_epaper** still draws the UI and the status bar, into a 540×960 RAM canvas
  (`createVirtual`). That canvas uses the .t5b page layout, so book pages are a plain
  `memcpy` as on the T5-2.13, and the UI code and fonts are shared.
- **Link clash:** FastEPD and bb_epaper both define a dozen helpers (pixel setters,
  UTF-8 to cp1252, the Group5 codec) with identical C++ signatures.
  `firmware/tools/fastepd_symbols.py` (PlatformIO extra script) renames them inside
  FastEPD only.
- `show()` rotates the portrait canvas into FastEPD's native 960×540 1-bpp buffer
  (`rotate1bpp`, lib/core, unit-tested). The refresh modes map to FastEPD calls:
  - Full: `fullUpdate(CLEAR_FAST)`
  - Fast: `fullUpdate(CLEAR_NONE)`
  - Partial: `partialUpdate()`, which diffs against FastEPD's "previous" buffer

## Firmware design notes

- **Portrait 540×960**, status bar 28 px, UI at scale 2 (`kUiScale`): the 250×122
  design at twice the size, with a second set of Ubuntu UI fonts (`*_2x.h`).
- **Partial refresh after deep sleep.** The panel keeps no image, and a page (64 KB)
  doesn't fit in RTC memory. On wake the app reads the page that is on the glass back
  from the card and redraws its status bar with the battery level it showed
  (`rtc.pageBattery`). It hands that to the display as "previous" (`assumeShown()`),
  so a page turn after deep sleep can still be partial. Cost: one extra SD read.
  Settings → Wake refresh: `rewrite`/`retain` do this, `full` doesn't.
- **One button:** tap = next, double tap = previous, hold = menu/select,
  long hold = back/sleep (`ClickDecoder` in lib/core, see `docs/usage.md`).
  A tap waits 300 ms for a possible second tap.
- **BOOT (GPIO0)** is the shift-register strobe, so the firmware can't read it.
  LilyGo's sleep example wakes on it (ext1); the reader doesn't, so only the user
  button wakes it.
- **Before deep sleep:** panel rails off, then every shift-register output low
  (LilyGo's `epd_poweroff_all()`, which also cuts the Molex/3.3 V header supply).
- **Partitions:** `default_16MB.csv` (2 × 6.5 MB app slots, OTA works). App 1.48 MB
  in 0.8.0.
- **Books are bigger:** 64.8 KB per page (vs 3.9 KB), but about 4× fewer pages at the
  default 28 px: Max Havelaar comes to about 55 MB. Page compression would be a format
  v2 (FastEPD has a Group5 decoder).

## To verify (checkpoint list in `docs/checkpoints.md`)

- Image upright with `kEpdRotation = 90`, and not mirrored. If it is mirrored, set
  `BB_PANEL_FLAG_MIRROR_X` or `_Y` in `fastepd_panel.cpp`.
- Clean partial updates with FastEPD's default 4 passes, and ghosting after N turns.
- Button idle level with only the internal pull-up; wake from deep sleep on GPIO21.
- Wake-to-page time: the PSRAM self-test at boot may add several hundred ms.
- Battery reading (ADC2, divider ×2); sleep current with the shift register cleared.
- SD at 20 MHz on these pins.

## Checkpoint results

- Rotation / mirroring: _
- Full / fast / partial refresh time, ghosting: _
- Button, wake, gestures feel: _
- Wake-to-page latency, sleep current: _
