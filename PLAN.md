# PLAN — T5-2.13 Pocket E-Reader

Executor: Claude Code. Owner: David. Work phase by phase; tick checkboxes, commit per phase, stop at every **🧑 HUMAN CHECKPOINT** (requires the physical board).

---

## 0. Key research findings (read first)

1. **FastEPD does not fit this board.** FastEPD drives *parallel* e-ink panels (M5Paper S3, LilyGo T5 S3 4.7", Inkplate, EPDiy V7). The T5-2.13 has an **SPI panel with a built-in controller** (SSD1680 / UC81xx).
2. **Use `bb_epaper` instead** — same author (Larry Bank), same design philosophy, written for 24-pin SPI panels. Relevant features: runtime panel selection, panel tester example, partial/fast/full refresh, compressed fonts + font converter, buffered *and* bufferless drawing.
3. **"T5-2.13" is several boards.** Known variants:

   | Variant | Panel | Resolution | Controller | bb_epaper candidate |
   |---|---|---|---|---|
   | T5 V2.3.1 / V2.4 (Q299) | DEPG0213BN | 122×250 B/W | SSD1680 | `EP213_122x250` |
   | T5 2.13 "4-gray" | GDEM0213B74 | 122×250 4-gray | SSD1680 | `EP213_122x250`, try `EP213Z_122x250_4GRAY` |
   | Older T5 2.13 | GDEW0213T5D-like | 104×212 B/W | UC8151 | `EP213_104x212` |

   → Phase 0 identifies the exact panel before anything else.
4. **Hardware constraints:** classic ESP32 (no PSRAM), 4 MB flash, ~320 KB SRAM. A 1-bpp framebuffer is only ~3.9 KB, so RAM is not the problem — **text layout and EPUB parsing are**. Hence: do the heavy lifting on the host.
5. **Expected pins (V2.3.1/V2.4 — verify in Phase 0):**
   - EPD: MOSI 23, SCK 18, CS 5, DC 17, RST 16, BUSY 4
   - TF card: SCK 14, MISO 2, MOSI 15, CS 13 (separate SPI bus; GPIO2/15 are strapping pins). LilyGo's wiki elsewhere says the bus is shared — verify.
   - Buttons: GPIO39 (user, input-only, RTC-capable), GPIO0 (BOOT, RTC-capable). Some variants have 4 buttons.
   - Battery ADC pin: **GPIO35**, divider 100k/100k (×2). Panel power enable: **GPIO12** (V2.4 only). See `docs/hardware.md`.

## 1. Architecture decision

**Host pre-renders, device only blits.**

```
EPUB/TXT ──► bookprep (Python, host) ──► book.t5b ──► SD card / LittleFS ──► firmware (ESP32)
             parse, hyphenate,          1-bpp page        read 1 page,
             justify, paginate,         bitmaps + TOC     write plane, refresh
             render with Pillow                           sleep
```

Rationale: proper typography (any TTF, kerning, NL/EN hyphenation, justification) without fighting the ESP32; trivial firmware; one page ≈ 3.9 KB raw, a 400-page book ≈ 1.6 MB — fine on SD. Trade-off: font size is fixed per generated file (mitigated in Phase 6).

`bb_epaper` is used for panel init, refresh modes, sleep, and UI text (status bar, menus) with its built-in/converted fonts.

## 2. Repo layout

```
t5-reader/
├─ CLAUDE.md                 # conventions + current phase (create in Phase 0)
├─ PLAN.md                   # this file; keep status updated
├─ pixi.toml                 # tasks: fw-build, fw-flash, fw-monitor, prep, test, lint
├─ docs/hardware.md          # verified pins, panel ID, measurements
├─ firmware/
│  ├─ platformio.ini         # esp32dev, Arduino, bb_epaper pinned
│  ├─ partitions.csv         # only if LittleFS fallback is needed
│  ├─ include/board.h        # board selector; pins + panel per board in include/boards/
│  └─ src/{main,display,input,book,storage,power,ui}.cpp/.h
└─ tools/bookprep/
   ├─ src/bookprep/{cli,extract,layout,render,t5b}.py
   ├─ fonts/                 # user-supplied TTF (license-compatible)
   └─ tests/
```

## 3. Conventions

- Pixi for everything (Python env **and** PlatformIO: `platformio` from conda-forge, PyPI fallback). ruff + pytest for the host tool.
- Pin `bb_epaper` to the latest release tag at bootstrap; record version in `platformio.ini` and `docs/hardware.md`.
- **Never trust API names in this plan blindly** — the bb_epaper method names below (`initIO`, `allocBuffer`, `writePlane`, `refresh(REFRESH_PARTIAL)`, `sleep`) are expected, not verified. Read `src/bb_epaper.h` and the examples (`epd_panel_tester`, partial-update example) before writing code.
- Firmware must compile in CI-like fashion (`pixi run fw-build`) without hardware. Hardware-dependent steps end at a checkpoint.
- No secrets in the repo (Wi-Fi creds via `secrets.h`, gitignored).

---

## Phase 0 — Bootstrap & hardware identification

- [x] Create repo skeleton, `pixi.toml` with tasks, `CLAUDE.md`, `.gitignore`
- [x] PlatformIO project: `board = esp32dev`, Arduino framework, PSRAM off, `bb_epaper` pinned, monitor 115200 *(pioarduino 55.03.39 = Arduino core 3.3.9; bb_epaper 2.1.11)*
- [x] Port bb_epaper's `epd_panel_tester` into `firmware/` with T5 pins from `board.h`; it should cycle the 3 candidate panel types and print the controller-ID check to serial *(`firmware/diag/panel_tester`: serial + button UI, 6 candidates, controller probed before any init sequence)*
- [x] Button test sketch: log GPIO39 and GPIO0 press/release with timestamps *(`firmware/diag/button_test`; also watches GPIO25/26/27/32/33 for a 4-button variant)*
- [x] SD test sketch: mount on HSPI (SCK 14, MISO 2, MOSI 15, CS 13), list root, write/read a 4 KB file, time a 3.9 KB random-offset read; also check whether the board boots normally with the card inserted (GPIO2/15 strapping pins), and whether SD and EPD actually share a bus *(`firmware/diag/sd_test`; bus check toggles SD pins and reads EPD pins)*
- [x] Look up battery ADC pin + divider in the LilyGo T5 repo; write findings to `docs/hardware.md` *(GPIO35, 100k/100k → ×2; also GPIO12 = panel power on V2.4)*

**🧑 HUMAN CHECKPOINT 0:** David reads the FPC ribbon label, flashes the tester + button sketch, and reports: panel label, which panel constant renders correctly, button GPIOs, board version, SD test output (FAT32-formatted card inserted). Claude Code records results in `docs/hardware.md` and fixes `board.h`.

*2026-10-13 (done except the FPC label): V2.3.1 board. Panel = `EP213Z_122x250` + RAM x offset 1 byte (the image sat 8 px low without it); full refresh 5.2 s (the reader waits for BUSY itself); fast refresh only with its preparation sent before the image; OTP partial does nothing, a custom LUT partial works (632 ms, `diag/refresh_test`). One button (GPIO39) + RST, no BOOT: one-button scheme. SD mounts, but must be out for flashing. Released as 1.0.0. Details in `docs/hardware.md`.*

**Done when:** one panel constant confirmed; buttons confirmed.

## Phase 1 — Display bring-up & layout calibration

- [x] `display.cpp`: init, clear, full / partial refresh wrappers, sleep *(keeps a copy of the shown image in RTC memory for partial refresh after deep sleep)*
- [x] Orientation: **landscape 250×122 (decided)**. No portrait support needed; hardcode rotation in `board.h` *(`kEpdRotation`; the calibration sketch can flip 90↔270 to find "up")*
- [x] **Calibration pattern**: firmware draws a known bitmap (corner markers, 1-px border, arrow pointing "up") from a raw 1-bpp buffer, to fix byte order, bit order, row stride and rotation of the on-disk page format *(generated by `pixi run devpages` through the real bookprep pipeline → `firmware/include/test_pages.h` + `out/devpages.t5b`; includes a bit-order staircase; `pixi run fw-flash calibration`)*
- [ ] Measure and log: full refresh time, partial refresh time *(calibration sketch prints them; numbers go in `docs/hardware.md` at the checkpoint)*
- [ ] Ghosting test: 30 partial updates of alternating text, then 1 full refresh *(implemented: NEXT short in the calibration sketch; needs the board)*

**🧑 HUMAN CHECKPOINT 1:** David confirms pattern orientation and judges ghosting after N partial updates → pick `FULL_REFRESH_EVERY` (start at 10).

**Done when:** raw page format spec (width, height, stride, bit order) is written in `docs/hardware.md`.

## Phase 2 — Host tool `bookprep` (no hardware needed)

- [x] `extract.py`: DRM-free EPUB → ordered chapters of paragraphs (`ebooklib` + `beautifulsoup4`); also plain `.txt`. Keep chapter titles for the TOC. Drop images in v1 (placeholder `[image]`) *(chapters split at TOC targets incl. `#fragment` anchors, so Gutenberg's size-split files work; `.txt`: Gutenberg header/footer stripped, `HOOFDSTUK`/`Chapter`/roman-numeral headings)*
- [x] Text normalisation: NFC, smart quotes, ellipsis, soft hyphens, nbsp; map characters missing from the font to fallbacks and report them *(`normalize.py`; Dutch elisions ’s/’t/’n; font coverage from the cmap via fontTools)*
- [x] `layout.py`: greedy line breaking with font metrics from Pillow (`ImageFont.getlength`), hyphenation via `pyphen` (language from EPUB metadata, `nl`/`en` default), optional justification, widow/orphan control light *(also: break after em/en dash, headings kept with the next line, chapters start a new page)*
- [x] Page model: text area = 250×122 minus margins (≈4 px) minus status bar (≈10 px, rendered **on device**, not baked in) *(now 14 px: a reading-progress line + text; at 13 px the page still holds 6 lines)*
- [x] `render.py`: Pillow 1-bit render (no dithering; threshold anti-aliasing at a tuned level) *(`--threshold N`, or `--mono` for FreeType hinted mono rendering; measured with the same hinting)*
- [x] `t5b.py`: binary format, little-endian *(byte-level spec: `docs/t5b-format.md`)*:
  ```
  header:  magic "T5B1", version u16, page_w u16, page_h u16, stride u16,
           page_count u32, toc_count u16, title[64] utf-8, author[64] utf-8
  toc:     toc_count × {start_page u32, title[48] utf-8}
  pages:   page_count × raw 1-bpp bitmap in the Phase-1 format (fixed size → page n at offset = header+toc+n*page_bytes)
  ```
  Fixed-size pages = O(1) seek, no index needed.
- [x] CLI: `pixi run prep book.epub --font fonts/X.ttf --size 13 --lang nl --out out/ --preview` → writes `.t5b` + preview PNGs of first 10 pages + a contact sheet
- [x] Tests: round-trip header, page offset math, hyphenation on NL/EN samples, character fallback report, golden-image test for one page *(57 tests, `pixi run test`)*

**🧑 HUMAN CHECKPOINT 2 (no board):** David reviews preview PNGs, picks font + size + margins. *(Comparison sheets: `out/checkpoint2/font-comparison.png` (threshold) and `font-comparison-mono.png`; 5 fonts × 11–14 px on Max Havelaar.)* **Done 2026-10-12: Ubuntu Regular 13 px, threshold rendering, default margins** (= bookprep defaults: 6 lines × ~38 chars).

## Phase 3 — Reader firmware v1

- [x] `storage.cpp`: mount SD (HSPI with Phase-0 pins) as the only storage; clear error screen if no card / no `/books`. LittleFS fallback (custom `partitions.csv`) only if Phase 0 SD test failed *(LittleFS fallback not built: only if Checkpoint 0 shows SD trouble)*
- [x] `book.cpp`: open `.t5b`, validate header vs. compiled page format, read page n into the framebuffer, read TOC
- [x] `input.cpp`: debounced buttons. Default mapping for 1 user button + BOOT: *(pure state machine in `lib/core/button.cpp`, unit-tested; in lists NEXT long = previous item with auto-repeat)*
  - GPIO39 short = next page · GPIO39 long (>600 ms) = previous page
  - GPIO0 short = menu · GPIO0 long = sleep now
  - (remap in `board.h` if the 4-button variant)
- [x] `ui.cpp`: status bar — page x/y, %, chapter short title, battery icon *(Ubuntu UI fonts; restyled 2026-10-12: progress line, rounded selection, key-chip hints, icon empty states. Every screen renders on the PC: `pixi run ui-preview`)*
- [x] Bookmark per book (hash of title+page_count) in NVS (`Preferences`); restore on boot
- [x] Refresh policy: partial for page turns, full every `FULL_REFRESH_EVERY` turns and on chapter change

**🧑 HUMAN CHECKPOINT 3:** read a real chapter on the device. Report latency, ghosting, legibility.

## Phase 4 — Power: sleep between page turns

Goal: e-paper keeps the image, so the MCU should deep-sleep after every turn.

- [x] After render: panel to deep sleep, ESP32 deep sleep; wake via `ext0` on GPIO39 and `ext1` (ALL_LOW, single pin) on GPIO0
- [x] Keep current book/page in RTC memory (fast path) + NVS (persistent)
- [x] On wake, detect short vs. long press (sample pin after wake) to decide next/prev
- [x] **Spike — partial refresh after deep sleep.** SSD16xx partial updates need the *previous* image in the controller's second RAM, which may be lost. Try in order, keep the first that gives clean results: *(all three implemented, switchable at runtime: Settings > Wake refresh = rewrite (2, default) / retain (1) / full (3); the shown image lives in RTC RAM. 2026-10-13: on David's panel every partial update is a hardware reset + custom LUT + old image from RTC RAM written again, i.e. "rewrite"; it works after deep sleep. Clean results / ghosting: Checkpoint 4)*
  1. Panel sleep mode that retains RAM, partial refresh as-is
  2. Re-write previous page into the "old" plane and new page into the "new" plane, then partial refresh (we can always re-read page n−1 from storage)
  3. Fallback: fast/full refresh on every wake
- [ ] Measure wake-to-image latency (target < 1.5 s) and sleep current; log to `docs/hardware.md` *(the reader logs `page ... at N ms` on serial; needs the board)*
- [x] Idle timeout (e.g. 10 min) → sleep screen (book title + "sleeping") with full refresh *(timer wake while a page is shown -> sleep screen; awake in menus -> same)*
- [x] Battery read + low-battery screen *(< 3.4 V; ignored below 2.5 V (USB without battery))*

**🧑 HUMAN CHECKPOINT 4:** David measures sleep current (USB meter / multimeter) and confirms page-turn feel.

## Phase 5 — Library & navigation

- [x] Book list from `/books/*.t5b` (title/author from headers), select with buttons
- [x] Menu: Resume · Chapters (TOC jump) · Go to % · Library · Settings (full-refresh interval, long-press time, idle timeout) · About (battery, version) *(plus Sleep; user guide: `docs/usage.md`)*
- [x] Menu text via bb_epaper fonts; convert one UI font with the bb_epaper font converter incl. Latin-1 (é, ë, ï, ü …) *(`pixi run ui-fonts`: bb_epaper's fontconvert built from the pinned library, Ubuntu 12/16 px, codepage 1252)*

## Phase 6 — Optional extensions (pick later)

- [x] Wi-Fi upload mode: menu item starts AP or STA + tiny web server to upload `.t5b` to SD; Wi-Fi off otherwise *(done 2026-10-12: own WPA2 network or home Wi-Fi via `secrets.h`; QR codes on the e-ink to join and to open the page; 4-digit session code; upload with validation, list with progress, delete; plus firmware update over Wi-Fi (OTA, min_spiffs partitions). See `docs/usage.md`)*
- [x] In-browser conversion (David, 2026-10-12): EPUB/TXT dropped on the reader's page are typeset on the phone (`firmware/web/bookprep.js`, a port of bookprep checked against it by `pixi run web-test`); fonts/size/spacing/weight/hyphenation chosen with a live device preview; the reader stores the original + settings so books can be restyled later from any phone, bookmark moved proportionally. Bundled: Ubuntu, Literata, Atkinson Hyperlegible, Merriweather; nl/en/fr hyphenation. Makes the next item mostly unnecessary.
- [ ] Two font sizes in one `.t5b` (page sets + position mapping by character offset)
- [ ] 4-gray anti-aliased rendering (only if the panel is the 4-gray variant; expect full refresh per page)
- [ ] On-device `.txt` layout with a bb_epaper compressed font (no host step)
- [ ] 3D-printed case → separate plan: `CASE_PLAN.md` (SolidPython2, `case/`). If external page-turn buttons are chosen there, add their GPIOs to `board.h` and the input mapping in Phase 3

## Phase 7 — Second board: LilyGo T5-4.7" S3 (example port, 2026-10-12)

David: "as an example, also add the LilyGO T5-4.7 inch S3". Untested on hardware;
notes in `docs/hardware-t5-47.md`, checks in `docs/checkpoints.md`.

- [x] Board selection: `include/board.h` picks `boards/t5_213.h` (default) or `boards/t5_47_s3.h` (`-DBOARD_T5_47_S3`); env `reader_t547` (ESP32-S3, 16 MB, OPI PSRAM, `default_16MB.csv`)
- [x] Display backend for parallel panels: FastEPD 2.2.0 custom panel (LilyGo shift-register scheme) in `fastepd_panel.cpp`; bb_epaper keeps drawing into a portrait 540×960 canvas, rotated on `show()` (`rotate1bpp`, lib/core, tested). Duplicate symbols between the two libraries renamed inside FastEPD (`firmware/tools/fastepd_symbols.py`)
- [x] One-button input: tap / double tap / hold / long hold mapped onto the NEXT/MENU keys (`ClickDecoder`, lib/core, tested); footer chips name the gestures
- [x] UI scale (`kUiScale`): same design at 2×, second UI font set, lists fill the screen, full-screen layouts centred. T5-2.13 screens checked pixel-identical to before
- [x] Partial refresh after deep sleep without RTC copy: the shown page is read back from the card (`display::assumeShown()`)
- [x] Converters: screen presets (`SCREENS` in `layout.py`, `screens` in `bookprep.js`, `--screen` for prep/devpages/web-dev); the reader reports its screen in `/api/info`, the page typesets for it; book upload limit scales with page size; OTA refuses the other chip's image
- [x] `pixi run ui-preview` renders both boards; `fw-dist` adds `t5-47-reader-*.bin`
- [ ] 🧑 Hardware check (if David gets the board): rotation/mirroring, refresh quality and timing, gestures, wake latency, sleep current

## Phase 8 — M5Stack M5PaperS3 and M5Paper (2026-10-13)

David: "add support for the m5 stack" → PaperS3, then "do both the s3 and the original".
Untested on hardware; notes in `docs/hardware-m5paper.md`, checks in `docs/checkpoints.md`.

- [x] Board headers `m5paper_s3.h` / `m5paper.h` from M5Unified/M5GFX pin tables; envs `reader_m5papers3`, `reader_m5paper` (`m5stack_paper` board, PSRAM)
- [x] Canvas backend split from its panel (`panel.h`): FastEPD preset for the PaperS3 (GPIO47 handed back to the SD card after init), own IT8951 driver for the M5Paper (M5GFX protocol, 4-bpp loads, DU/GL16/GC16), SD sharing the IT8951's SPI bus
- [x] Touch: GT911 driver, `TouchTracker` gestures (lib/core, tested), per-screen mapping (`touchKey()`): tap zones and swipes when reading, tappable rows / footer buttons / Go to bar elsewhere; touch UI metrics (bigger rows and buttons), T5 screens pixel-identical
- [x] M5Paper 3-way switch: a PREV button role next to NEXT/MENU
- [x] Power: light sleep between pages (touch INT on the PaperS3 can't wake deep sleep), switch-off when idle/Sleep (PaperS3 PWROFF pulses, M5Paper GPIO2), battery re-read after naps
- [x] Screen id `t5-47` → `4.7in` (shared by three boards); board tag in every image, Wi-Fi updates for another board refused; `fw-dist`/release notes list all four boards
- [ ] 🧑 Hardware check (if David gets one): rotation + touch alignment, IT8951 bring-up, refresh quality, SD on the PaperS3, light-sleep current, power-off

## Phase 9 — Ideas from the Pala One firmware (2026-10-13)

David compared with [pala-one-firmware](https://github.com/PaulLagier/pala-one-firmware)
(the project that inspired this one): "only copy the ideas, not any code". Skipped:
firmware updates from GitHub, apps, to-do list, Heltec hardware. Untested on hardware;
checks in `docs/checkpoints.md` → "Phase 9 features", user guide `docs/usage.md`.

- [x] Browser installer: `pixi run installer` → `out/installer/`, one manifest per board with bootloader/partitions/boot_app0/app at their own offsets, so NVS (settings, positions, statistics) survives; deployed to GitHub Pages by the release workflow, page-only changes by `installer.yml` (firmware of the latest release). First on ESP Web Tools 10.4.0; since 2026-10-14 its own page on esptool-js 0.7.0, whose reset sets DTR and RTS together (0.6, inside ESP Web Tools, couldn't reset David's T5-2.13 into its bootloader on Linux)
- [x] Home Wi-Fi from the page (NVS; password write-only), comes before `secrets.h`
- [x] Bookmarks: up to 100 per book in `/books/.marks/<stem>.txt` (`PageList`, lib/core, tested), Bookmarks screen, ribbon in the status bar, remapped with the position on restyle
- [x] Search on the page: re-typesets the stored original with its stored options, page snippets, page view from `/api/page`, "Read from here" sets the position
- [x] Sleep screen setting: book / cover (optional `T5CV` block after the pages, `docs/t5b-format.md`; from the EPUB in both converters) / pictures (`/sleep/*.bin`, full-screen 1-bpp, made in the page's picture editor: fit/fill, zoom, rotate, brightness/contrast, sharpen, Atkinson / Floyd-Steinberg / ordered / threshold; covers: sharpen + Atkinson, parity-tested JS vs Python) / keep page
- [x] Reading statistics (`ReadingStats`, lib/core, tested): pages, time (≤ 3 min per page), days read, streak; RTC RAM + NVS; clock set from the phone (`/api/time`), kept through power-off by the M5 boards' BM8563 (`rtc_clock.cpp`, `rtc_time` in lib/core, tested)
- [x] Lock: menu item, hold action, lock when asleep; locked presses go back to sleep without a refresh; unlock = the board's hold gesture
- [x] Status bar: full / minimal / hidden
- [x] Reading aids: OpenDyslexic bundled (unmodified WOFF2, OFL with Reserved Font Name), bionic reading (`--bionic`, page checkbox)
- [x] Library folders (one level, `/books/<folder>/`), move/upload into a folder from the page; factory reset from the page (typed confirmation)
- [x] UI language English / Dutch (`i18n.h` table; the page follows the reader)
- [x] Hold gesture remappable (sleep / bookmark / lock / menu); device name shown above the library
- [ ] 🧑 Hardware check: see `docs/checkpoints.md` → "Phase 9 features"

## After 1.0 (releases 1.1.0 on 2026-10-14, 1.2.0 on 2026-10-20)

- [x] Pictures: Atkinson dithering by default (also Floyd-Steinberg, ordered, threshold), sharpening, brightness/contrast in the picture editor; covers sharpened + Atkinson in both converters (parity-tested); contrast measured on the photo only in "Fit"
- [x] Settings → Flip screen: image 180 degrees (rotation 90 <-> 270), the next refresh full; touch and the M5Paper's switch turn along
- [x] Installer on esptool-js 0.7.0 (DTR/RTS set together; ESP Web Tools' 0.6 couldn't reset the T5-2.13 into its bootloader on Linux), "Erase everything" box, Linux section (Snap Chromium `raw-usb`, `dialout`); page-only updates from main (`installer.yml`)
- [ ] 🧑 Flip screen on the T5-2.13 (expected fine: the 1-byte RAM offset is symmetric)
- [x] Sleep screen "picture + book": a square picture (the screen's short side, `kSquareBytes` in `/sleep`, told apart from full-screen ones by size) left / on top, title, author and progress beside it; the editor's *Shape* (full screen / square, starts on Fill); falls back to "book" without square pictures
- [ ] 🧑 "Picture + book" on the T5-2.13: make a square picture on the page, check the layout and that "pictures" keeps showing only the full-screen ones

---

## Decisions

- Orientation: **landscape 250×122** (≈35–40 chars × ~8 lines). Page format, layout and UI target landscape only.
- Storage: **SD card** (David has one). Books in `/books/*.t5b`. LittleFS only as emergency fallback if SD proves unreliable.

## Open questions for David

1. Exact board version / panel label (Phase 0 answers this)
2. Dutch, English or both as default hyphenation language?
3. Font preference (needs a license that allows embedding/rendering; e.g. a SIL OFL font)

## Risks

| Risk | Mitigation |
|---|---|
| Wrong panel constant can misdrive the panel | Phase 0 panel tester with controller-ID check before any app code |
| Partial refresh broken after deep sleep | Phase 4 spike with 3 fallbacks |
| SD on strapping pins (GPIO2/15) causes boot issues | Test boot with card inserted; LittleFS fallback |
| Only one user button → clumsy UX | Short/long/BOOT mapping; external buttons on free GPIOs in Phase 6. One-button boards: tap/double tap/hold/long hold (Phase 7) |
| Tiny screen readability | Preview PNGs before flashing; tune size/margins in Checkpoint 2 |
