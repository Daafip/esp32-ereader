# Hardware notes: M5Stack M5PaperS3 and M5Paper

Two more build targets, added after the T5-4.7" port. Nothing here has run on a board
yet: **M5** = taken from M5Stack's own libraries, **VERIFY** = to check on a board.

Sources: M5Unified (`src/M5Unified.inl` pin tables, `src/utility/Power_Class.inl`) and
M5GFX (`src/M5GFX.cpp` board autodetect, `src/lgfx/v1/panel/Panel_IT8951.inl`), both at
github.com/m5stack, commits of 2026-10-13. Also FastEPD 2.2.0's `BB_PANEL_M5PAPERS3`.

Both have the same screen as the T5-4.7" (ED047TC1, 960×540, used portrait 540×960),
so books for one open on all three (`--screen 4.7in`). Both have touch.

## M5PaperS3 (env `reader_m5papers3`)

| Item | Value | Status |
|---|---|---|
| MCU | ESP32-S3R8, 16 MB flash, 8 MB OPI PSRAM | M5 |
| Panel | parallel, FastEPD preset `BB_PANEL_M5PAPERS3`: D0..D7 6, 14, 7, 12, 9, 11, 8, 10; PWR 46, SPV 17, CKV 18, SPH 13, OE 45, LE 15, CL 16 | M5 |
| Touch | GT911, I2C SDA 41 / SCL 42, INT 48 (not an RTC GPIO) | M5 |
| SD | SCK 39, MOSI 38, MISO 40, CS 47 | M5 |
| Battery | ADC GPIO3, ×2; charging status GPIO4 (low = charging, not used) | M5 |
| Power off | PWROFF_PULSE GPIO44: kept low, 5 low/high pulses switch the device off | M5 |
| Buttons | none (the power button only switches power) | M5 |
| RTC | BM8563 at 0x51 on the touch I2C bus: keeps UTC while switched off (statistics) | M5 |
| Other | LED GPIO0, buzzer GPIO21, BMI270 on I2C (not used) | M5 |

- **GPIO47 clash:** FastEPD's preset uses GPIO47 as the dummy D/C pin the S3's LCD
  peripheral insists on, and GPIO47 is also the SD card's CS. `fastepd_panel.cpp`
  reconnects GPIO47 to plain GPIO output right after the panel init (the LCD bus never
  needs D/C), so the SD library owns it. VERIFY: SD and display both work.
- **Sleep:** the touch INT can't wake the chip from deep sleep. So the reader light
  sleeps between pages (RAM, the panel's buffers and the touch controller stay alive;
  touch wakes it). When idle, or with Menu → Sleep, it draws the sleep screen and
  switches off. The power button turns it on again, and it reopens the book at the
  bookmark. On USB power it can't switch off, so it waits for a touch and restarts.

## M5Paper (env `reader_m5paper`)

| Item | Value | Status |
|---|---|---|
| MCU | ESP32-D0WDQ6-V3, 16 MB flash, 8 MB PSRAM (PlatformIO board `m5stack_paper`) | M5 |
| Panel | IT8951 controller on SPI: SCK 14, MOSI 12, MISO 13, CS 15, HRDY/busy 27; GPIO23 powers it (M5GFX pulses it as reset) | M5 |
| SD | same SPI bus, CS 4 | M5 |
| Touch | GT911, I2C SDA 21 / SCL 22, INT 36 (input-only) | M5 |
| Buttons | 3-way switch: GPIO37 (BtnA), 38 (press, BtnB), 39 (BtnC); active low, external pull-ups | M5 / VERIFY which end is "down" |
| Battery | ADC GPIO35, ×2 | M5 |
| Power hold | GPIO2: high keeps the board on when on battery, low switches it off | M5 |
| RTC | BM8563 at 0x51 on the touch I2C bus: keeps UTC while switched off (statistics) | M5 |

- **Own IT8951 driver** (`src/it8951_panel.cpp`, about 200 lines). FastEPD 2.2.0
  always refreshes an IT8951 with its flashing full-update mode, so I followed M5GFX's
  protocol instead:
  - Before every 16-bit word, wait for HRDY. A transfer starts with a preamble word:
    0x6000 for a command, 0x0000 for data, 0x1000 for a read.
  - Init: read the device info (image buffer address), then SYS_RUN, packed write and
    the target address.
  - Pixels go as 4 bpp, big-endian. That is the load format M5GFX uses on this board;
    a 1-bpp load would depend on a bit-order setting I can't test.
  - Refresh modes: Full = GC16 (2), Fast = GL16 (3), Partial = DU (1).
  - The controller keeps the image in its own RAM, so partial updates need no
    "previous" buffer on the ESP32.
- **Shared SPI:** the SD card and the IT8951 both use Arduino's global `SPI` object
  (`kSdSharedSpi`), each with its own CS.
- **Sleep:** light sleep between pages with the IT8951 in standby. Touch (GPIO36) or
  the switch wakes it. "Sleep" switches off by pulling GPIO2 low; pressing the switch
  turns it on again (VERIFY: which press powers it on). On USB: it waits for input and
  restarts.

## Touch

- `src/touch_gt911.cpp`: GT911 at I2C 0x5D or 0x14. Status 0x814E (bit 7 = new report,
  low nibble = points), first point at 0x814F; writing 0 to the status clears it. On the
  M5 boards INT stays low until the status is cleared, so the reader only talks to the
  controller when INT is low or a finger is down. That same level is the light-sleep
  wake source.
- Raw coordinates are portrait 540×960 (M5GFX config). They should match the canvas
  when `kEpdRotation` matches M5's portrait orientation (M5GFX: panel offset rotation 3,
  touch offset rotation 1). If the image is upside down, flip `kEpdRotation` *and*
  `kTouchFlip` together. VERIFY.
- Gestures (`TouchTracker`, lib/core, tested): tap, long press (the "Long press"
  setting), and swipes of at least 80 px. What they do: `docs/usage.md`.

## To verify

- Image upright and not mirrored (`kEpdRotation` 270), touch on the right spot (`kTouchFlip`).
- PaperS3: the SD card works after the panel init (the GPIO47 hand-over).
- M5Paper: the IT8951 answers (serial: `IT8951: panel 960x540, image buffer at ...`),
  DU partial updates are clean, and the 4-bpp load shows the right pixels.
- Light-sleep current and wake time per page, and the switch-off on battery.
- M5Paper: which switch direction is "down" (`kBtnNext` 39 / `kBtnPrev` 37).
- The BM8563 answers at 0x51 and keeps the time while off (`src/rtc_clock.cpp`;
  serial: `clock: ... from the RTC` after switching on).

## Checkpoint results

- M5PaperS3: rotation / touch, refresh, SD, sleep and power-off: _
- M5Paper: IT8951 info line, rotation / touch, switch directions, refresh, sleep and power-off: _
