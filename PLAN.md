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
│  ├─ include/board.h        # all pins + panel constant in ONE place
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

**Done when:** one panel constant confirmed; buttons confirmed.

## Phase 1 — Display bring-up & layout calibration

- [ ] `display.cpp`: init, clear, full / partial refresh wrappers, sleep
- [ ] Orientation: **landscape 250×122 (decided)**. No portrait support needed; hardcode rotation in `board.h`
- [ ] **Calibration pattern**: firmware draws a known bitmap (corner markers, 1-px border, arrow pointing "up") from a raw 1-bpp buffer, to fix byte order, bit order, row stride and rotation of the on-disk page format
- [ ] Measure and log: full refresh time, partial refresh time
- [ ] Ghosting test: 30 partial updates of alternating text, then 1 full refresh

**🧑 HUMAN CHECKPOINT 1:** David confirms pattern orientation and judges ghosting after N partial updates → pick `FULL_REFRESH_EVERY` (start at 10).

**Done when:** raw page format spec (width, height, stride, bit order) is written in `docs/hardware.md`.

## Phase 2 — Host tool `bookprep` (no hardware needed)

- [ ] `extract.py`: DRM-free EPUB → ordered chapters of paragraphs (`ebooklib` + `beautifulsoup4`); also plain `.txt`. Keep chapter titles for the TOC. Drop images in v1 (placeholder `[image]`)
- [ ] Text normalisation: NFC, smart quotes, ellipsis, soft hyphens, nbsp; map characters missing from the font to fallbacks and report them
- [ ] `layout.py`: greedy line breaking with font metrics from Pillow (`ImageFont.getlength`), hyphenation via `pyphen` (language from EPUB metadata, `nl`/`en` default), optional justification, widow/orphan control light
- [ ] Page model: text area = 250×122 minus margins (≈4 px) minus status bar (≈10 px, rendered **on device**, not baked in)
- [ ] `render.py`: Pillow 1-bit render (no dithering; threshold anti-aliasing at a tuned level)
- [ ] `t5b.py`: binary format, little-endian:
  ```
  header:  magic "T5B1", version u16, page_w u16, page_h u16, stride u16,
           page_count u32, toc_count u16, title[64] utf-8, author[64] utf-8
  toc:     toc_count × {start_page u32, title[48] utf-8}
  pages:   page_count × raw 1-bpp bitmap in the Phase-1 format (fixed size → page n at offset = header+toc+n*page_bytes)
  ```
  Fixed-size pages = O(1) seek, no index needed.
- [ ] CLI: `pixi run prep book.epub --font fonts/X.ttf --size 13 --lang nl --out out/ --preview` → writes `.t5b` + preview PNGs of first 10 pages + a contact sheet
- [ ] Tests: round-trip header, page offset math, hyphenation on NL/EN samples, character fallback report, golden-image test for one page

**🧑 HUMAN CHECKPOINT 2 (no board):** David reviews preview PNGs, picks font + size + margins.

## Phase 3 — Reader firmware v1

- [ ] `storage.cpp`: mount SD (HSPI with Phase-0 pins) as the only storage; clear error screen if no card / no `/books`. LittleFS fallback (custom `partitions.csv`) only if Phase 0 SD test failed
- [ ] `book.cpp`: open `.t5b`, validate header vs. compiled page format, read page n into the framebuffer, read TOC
- [ ] `input.cpp`: debounced buttons. Default mapping for 1 user button + BOOT:
  - GPIO39 short = next page · GPIO39 long (>600 ms) = previous page
  - GPIO0 short = menu · GPIO0 long = sleep now
  - (remap in `board.h` if the 4-button variant)
- [ ] `ui.cpp`: status bar — page x/y, %, chapter short title, battery icon
- [ ] Bookmark per book (hash of title+page_count) in NVS (`Preferences`); restore on boot
- [ ] Refresh policy: partial for page turns, full every `FULL_REFRESH_EVERY` turns and on chapter change

**🧑 HUMAN CHECKPOINT 3:** read a real chapter on the device. Report latency, ghosting, legibility.

## Phase 4 — Power: sleep between page turns

Goal: e-paper keeps the image, so the MCU should deep-sleep after every turn.

- [ ] After render: panel to deep sleep, ESP32 deep sleep; wake via `ext0` on GPIO39 and `ext1` (ALL_LOW, single pin) on GPIO0
- [ ] Keep current book/page in RTC memory (fast path) + NVS (persistent)
- [ ] On wake, detect short vs. long press (sample pin after wake) to decide next/prev
- [ ] **Spike — partial refresh after deep sleep.** SSD16xx partial updates need the *previous* image in the controller's second RAM, which may be lost. Try in order, keep the first that gives clean results:
  1. Panel sleep mode that retains RAM, partial refresh as-is
  2. Re-write previous page into the "old" plane and new page into the "new" plane, then partial refresh (we can always re-read page n−1 from storage)
  3. Fallback: fast/full refresh on every wake
- [ ] Measure wake-to-image latency (target < 1.5 s) and sleep current; log to `docs/hardware.md`
- [ ] Idle timeout (e.g. 10 min) → sleep screen (book title + "sleeping") with full refresh
- [ ] Battery read + low-battery screen

**🧑 HUMAN CHECKPOINT 4:** David measures sleep current (USB meter / multimeter) and confirms page-turn feel.

## Phase 5 — Library & navigation

- [ ] Book list from `/books/*.t5b` (title/author from headers), select with buttons
- [ ] Menu: Resume · Chapters (TOC jump) · Go to % · Library · Settings (full-refresh interval, long-press time, idle timeout) · About (battery, version)
- [ ] Menu text via bb_epaper fonts; convert one UI font with the bb_epaper font converter incl. Latin-1 (é, ë, ï, ü …)

## Phase 6 — Optional extensions (pick later)

- [ ] Wi-Fi upload mode: menu item starts AP or STA + tiny web server to upload `.t5b` to SD; Wi-Fi off otherwise
- [ ] Two font sizes in one `.t5b` (page sets + position mapping by character offset)
- [ ] 4-gray anti-aliased rendering (only if the panel is the 4-gray variant; expect full refresh per page)
- [ ] On-device `.txt` layout with a bb_epaper compressed font (no host step)
- [ ] 3D-printed case → separate plan: `CASE_PLAN.md` (SolidPython2, `case/`). If external page-turn buttons are chosen there, add their GPIOs to `board.h` and the input mapping in Phase 3

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
| Only one user button → clumsy UX | Short/long/BOOT mapping; external buttons on free GPIOs in Phase 6 |
| Tiny screen readability | Preview PNGs before flashing; tune size/margins in Checkpoint 2 |
