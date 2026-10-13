# Hardware notes — LilyGo T5-2.13

(The other boards have their own pages: LilyGo T5-4.7" S3 in `docs/hardware-t5-47.md`,
M5PaperS3 and M5Paper in `docs/hardware-m5paper.md`.)

Status legend: **SCHEMATIC** = from LilyGo's repo (README, `T5_2.13.pdf` schematic
V2.3, examples) as of 2026-07; **VERIFIED** = measured on David's board;
**OPEN** = still unknown.

Source: <https://github.com/Xinyuan-LilyGO/LilyGo-T5-Epaper-Series> (commit `0118965`, 2026-07-22).

## Board

| Item | Value | Status |
|---|---|---|
| MCU | ESP32-D0WDQ6 (classic), 4 MB flash, no PSRAM | SCHEMATIC |
| USB-UART | CP2104 (schematic V2.3) / CH9102 (README, newer batches) | SCHEMATIC |
| Board version | V2.3.1 or V2.4 | OPEN |
| Panel | DEPG0213BN (LilyGo product Q229) or GDEM0213B74 (Q300), both SSD1680 122×250 | OPEN |

V2.4 vs V2.3.1: V2.4 adds GPIO12 = panel power enable, a BOOT button and a better
antenna; deep sleep ≈ 30 µA (LilyGo measurement, both panels).

## Pins

| Function | GPIO | Notes | Status |
|---|---|---|---|
| EPD MOSI | 23 | VSPI | SCHEMATIC |
| EPD SCK | 18 | VSPI | SCHEMATIC |
| EPD CS | 5 | | SCHEMATIC |
| EPD DC | 17 | | SCHEMATIC |
| EPD RST | 16 | | SCHEMATIC |
| EPD BUSY | 4 | SSD16xx: high = busy | SCHEMATIC |
| EPD power enable | 12 | **V2.4 only**, drive HIGH before talking to the panel. Strapping pin (MTDI): only drive after boot | SCHEMATIC |
| SD SCK | 14 | HSPI; separate nets from the EPD bus in the V2.3 schematic | SCHEMATIC |
| SD MISO | 2 | strapping pin, 10k pull-up | SCHEMATIC |
| SD MOSI | 15 | strapping pin, 10k pull-up | SCHEMATIC |
| SD CS | 13 | 10k pull-up | SCHEMATIC |
| Button USER | 39 | input-only, no internal pull-up, RTC GPIO3 (ext0/ext1 wake OK) | SCHEMATIC |
| Button BOOT | 0 | RTC GPIO11 (wake OK). Holding it at reset enters download mode | SCHEMATIC |
| Battery ADC | 35 | ADC1_CH7, divider R42/R43 = 100k/100k → V_bat = 2 × V_pin | SCHEMATIC |
| Green LED | 19 | | SCHEMATIC |

Free header GPIOs (for external page-turn buttons, Phase 6): 19 (LED), 21, 22, 25,
26, 27, 32, 33, 34 (input-only), 12 (V2.3.1 only).

### Gotchas

- LilyGo: *"You cannot upload software when the SD card is inserted, so remove the
  SD card before uploading."* (GPIO2 must be low/floating to enter download mode.)
  Booting normally with the card in is expected to work; the SD test confirms it.
- Battery reading as in LilyGo's factory firmware: average 16 × `analogReadMilliVolts(35)`,
  multiply by 2, clamp at 4200 mV.

## Display library

- `bb_epaper` **2.1.11 + upstream commit `1d92424`** (2026-07-09), pinned by commit
  hash in `firmware/platformio.ini`. The only changes after the 2.1.11 tag up to that
  commit are M5 Paper Mono support and a one-line font fix: 2.1.11 skips glyphs that
  are 1 px wide in custom fonts, so `i . : '` vanish from small menu fonts.
  Licence: **GPL-3.0-or-later**, so the firmware binary is GPL as well if we ever
  distribute it.
- **Bug, still on upstream `main`:** in buffered mode the built-in `FONT_6x8` draws every
  character of a string at the same x (it advances `iCursorX` but draws at a local `x`).
  Workaround: `display::drawText6x8()` draws one character at a time. Found with
  `pixi run ui-preview`. Could be reported upstream.
- When reading library source, use the pinned commit, not `main`: `main` already has
  panel types (e.g. `EP213B_122x250_4GRAY`) that our version doesn't.
- Verified API facts (from the 2.1.11 source):
  - `initIO()` immediately sends the panel's full init sequence. Do a hardware reset
    first if the panel may be in deep sleep.
  - After `sleep(DEEP_SLEEP)` (SSD16xx command 0x10 = 0x03) the controller only wakes
    on a hardware reset (`wake()` or toggling RST). `sleep(LIGHT_SLEEP)` (0x10 = 0x01) is
    "deep sleep mode 1", which keeps RAM: relevant for the Phase 4 spike.
  - `testPanelType()` = toggle RST, read BUSY: high → UC81xx, low → SSD16xx.
  - `refresh(REFRESH_PARTIAL)` re-sends the panel's partial init sequence, then
    activates. Partial uses RAM 0x24 (new, `PLANE_0`) against RAM 0x26 (old, `PLANE_1`).
    `writePlane(PLANE_0_TO_1)` sends the local buffer to the "old" RAM, so a single
    3.9 KB buffer is enough: write old page → `PLANE_0_TO_1`, new page → `PLANE_0`.
  - Custom fonts (fontconvert output): `y` is the baseline; text is converted from
    UTF-8 to codepage 1252. `getStringBox()` converts into an 80-byte stack buffer and
    `drawString()` into 256 bytes, both unchecked: keep strings short.
  - Built-in fonts: `y` is the top row; characters index the glyph table as `c - 32`
    unchecked, so pass ASCII only (`asciiFold()`).
  - `pfnSetPixelFast` has no bounds check: fit text before drawing.

## Firmware design notes

- **RTC memory** (survives deep sleep, 8 KB): the image on the panel (3904 B, for the
  "old" plane of a partial refresh after wake), current book path/page/chapter, which
  screen is shown, refresh counters. NVS keeps settings, last book and one bookmark
  per book (written on every page turn; NVS skips unchanged values).
- **Wake refresh strategies** (Settings → Wake refresh), the Phase 4 spike:
  - `rewrite` (default): panel power cut in deep sleep (V2.4, GPIO12 held low); on
    wake the previous image is sent from RTC RAM as "old", then a partial refresh.
  - `retain`: panel stays powered in its RAM-keeping sleep, RST/CS held high through
    ESP32 deep sleep; only the new image is sent. Depends on the controller keeping
    RAM through the hardware reset that wakes it: unverified.
  - `full`: fast refresh (full if the panel has none) on every page turn.
- **Wake-up**: NEXT = ext0 (GPIO39, low), MENU = ext1 (GPIO0, all-low, single pin,
  RTC pull-up on). The idle timer (Settings → Idle sleep) only runs while a page is
  shown; when it fires, the sleep screen is drawn and only buttons wake the reader.
- **Unverified on hardware**: that waking by holding GPIO0 low doesn't enter download
  mode (strapping pins are normally only sampled at power-on/chip reset), and the
  sleep current with the pins held.

- **Flash use** (0.10.0): app 1.63 MB of 1.9 MB, of which ~410 KB is the web page, the
  converter, five fonts and nl/en/fr hyphenation patterns (`include/web_assets.h`).
  About 300 KB left: the next big feature for this board may need a smaller page.
- **Partitions** (reader env): `min_spiffs.csv`, two 1.9 MB app slots (Wi-Fi and the
  web server need more than the default 1.25 MB; the second slot enables firmware
  updates over Wi-Fi). NVS stays at 0x9000 in every env, so settings survive
  flashing a diagnostic sketch in between.

## Candidate panel constants (bb_epaper 2.1.11)

| Constant | Size | Controller | Comment |
|---|---|---|---|
| `EP213_122x250` | 122×250 | SSD16xx | Waveshare-style init; full + partial, no fast |
| `EP213B_122x250` | 122×250 | SSD16xx | GDEY0213B74; full + fast + partial |
| `EP213ZZ_122x250` | 122×250 | SSD16xx | "LilyGo T3S3": as `EP213_122x250` with RAM x-offset 1 |
| `EP213Z_122x250` | 122×250 | SSD16xx | CrowPanel; full + fast + partial |
| `EP213Z_122x250_4GRAY` | 122×250 | SSD16xx | 4-gray mode (full refresh only) |
| `EP213_104x212` | 104×212 | UC81xx | old Inky pHAT; full only |

## Page format (provisional, to be confirmed in Phase 1)

With `setRotation(90|270)` bb_epaper keeps its local buffer in the **rotated
(landscape) orientation** and rotates while sending (`bbepWriteImage`). From the
2.1.11 source (`bbepSetPixel2Clr`, `bbepWriteImage`):

| Property | Value |
|---|---|
| width × height | 250 × 122 (landscape) |
| row stride | 32 bytes (`(250 + 7) / 8`) |
| page size | 32 × 122 = **3904 bytes** |
| order | row-major, top row first |
| bit order | MSB first: pixel x is bit `0x80 >> (x & 7)` of byte `x >> 3` |
| polarity | **1 = white, 0 = black** |
| padding | 6 bits at the end of each row, written as 1 (white) |

So a `.t5b` page can be `memcpy`'d into `epd.getBuffer()` and sent with
`writePlane()`. Which physical edge is "up" (90 vs 270) is a device setting
(`kEpdRotation` in `boards/t5_213.h`) and doesn't change the file format. Phase 1's calibration
pattern confirms all of this on the real panel.

## Checkpoint results

Filled in from David's reports; see `docs/checkpoints.md` for what to run.

### Checkpoint 0 (hardware identification)

- Board version / FPC label: _
- Controller-ID line from the panel tester: _
- Panel constant that renders correctly: _
- Panel tester RESULT lines: _
- Buttons (GPIO, idle level, bounce): _
- SD test output (bus check, mount speed, random read timing, boot with card in): _

### Checkpoint 1 (calibration)

- Upright rotation (90/270): _
- Calibration page correct (border, corners, staircase `\\`): _
- Full / fast / partial refresh time: _
- Ghosting after N partial updates → `FULL_REFRESH_EVERY` (Settings default): _

### Checkpoint 3/4 (reading, power)

- Wake-to-image latency per strategy (rewrite / retain / full): _
- Deep sleep current (rewrite / retain): _
- Long-press feel (threshold): _
