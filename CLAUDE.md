# T5-2.13 pocket e-reader

ESP32 e-reader on a LilyGo T5-2.13 (SSD1680-class 122×250 SPI e-paper, landscape
250×122). The host pre-renders books to 1-bpp page bitmaps (`.t5b`); the device only
blits pages. Full plan and phase status: `PLAN.md`. Hardware facts: `docs/hardware.md`.
More build targets (Phases 7-8): LilyGo T5-4.7" S3 (ESP32-S3, 960×540 parallel panel
via FastEPD, one button; `docs/hardware-t5-47.md`), M5Stack M5PaperS3 (same panel via
FastEPD's preset, touch only) and M5Paper (ESP32 + IT8951 controller with our own driver,
touch + 3-way switch; `docs/hardware-m5paper.md`). The 4.7" boards use portrait 540×960.

## Current phase

Phases 0-5 and 7-9 are written and build (7: T5-4.7" S3, 8: M5PaperS3/M5Paper,
9: ideas from the Pala One firmware). **The T5-2.13 runs (David's V2.3.1 board,
2026-10-13, release 1.0.0; 1.1.0 on 2026-10-14; 1.2.0 and 1.3.0 on 2026-10-20)**: panel, one button, SD, Wi-Fi transfer, partial refresh
with a custom LUT; results in `docs/hardware.md`. The other three boards have never
run. Open checks: `docs/checkpoints.md` (ghosting, power, the other boards, Phase 9).
T5-2.13: take the SD card out before flashing (uploads fail with it in). Checkpoint 2 is done: Ubuntu Regular 13 px
(bookprep default). Phase 6 is optional; Wi-Fi transfer and in-browser conversion
are done. UI look: check `pixi run ui-preview` after
any change to `ui.cpp` (both boards; T5-2.13 screens should stay pixel-identical unless
intended); the web page opens in demo mode straight from disk (`?screen=4.7in`).

## Commands (everything through pixi)

| Task | Command |
|---|---|
| Build all firmware envs | `pixi run fw-build` |
| Firmware unit tests (PC) | `pixi run fw-test` (`firmware/lib/core`, Unity, native env) |
| Render all screens (PC) | `pixi run ui-preview` -> `out/ui-preview/<board>/` (`t5-213`, `t5-47`, `m5paper` = touch UI) |
| Flash one env | `pixi run fw-flash reader` (`calibration`, `panel_tester`, `button_test`, `sd_test`; `reader_t547`, `reader_m5papers3`, `reader_m5paper`) |
| Images for browser/OTA flashing | `pixi run fw-dist` -> `out/firmware/` |
| Browser installer (keeps NVS) | `pixi run installer` -> `out/installer/` (after `fw-build`) |
| Release on GitHub | bump `FW_VERSION`, then `git tag vX.Y.Z && git push origin vX.Y.Z` (`.github/workflows/release.yml` builds and attaches the images) |
| Serial monitor | `pixi run fw-monitor` |
| Convert a book | `pixi run prep book.epub --font X.ttf --out out/ --preview` (`--screen 4.7in`) |
| Font comparison | `pixi run font-sheet book.epub --font a.ttf --sizes 11 12 13` |
| Regenerate generated headers | `pixi run devpages` (test_pages.h), `pixi run ui-fonts` (fonts/), `pixi run web` (web_index.h from `firmware/web/index.html`) |
| Host tests / lint | `pixi run test`, `pixi run lint`, `pixi run fmt` |
| Browser converter tests | `pixi run web-test` (Node parity + headless Chromium) |
| Web page without the reader | `pixi run web-dev` (`--lan` for a phone, `--screen 4.7in`, `--card-mb 5` for a full card) |

## Conventions

- All pins, the panel and the screen geometry live in `firmware/include/boards/<board>.h`
  (selected by `board.h`), nowhere else. A screen's size and converter defaults also
  appear in `SCREENS` (`bookprep/layout.py`) and `screens` (`firmware/web/bookprep.js`):
  change all three together.
- Display backends: `display_bbep.cpp` (SPI panels with their own RAM: T5-2.13) and
  `display_canvas.cpp` (portrait RAM canvas, rotated onto a `panel.h` driver:
  `fastepd_panel.cpp` for T5-4.7"/PaperS3, `it8951_panel.cpp` for M5Paper). FastEPD.h and
  bb_epaper.h never go in one translation unit; `firmware/tools/fastepd_symbols.py`
  renames their duplicate symbols at link. FastEPD is pinned to 2.2.0: read
  `firmware/.pio/libdeps/reader_t547/FastEPD/src`. M5 facts: M5Unified/M5GFX sources.
- Touch boards (`BOARD_HAS_TOUCH`): `touch_gt911.cpp` reports fingers, `TouchTracker`
  (lib/core) makes gestures, `app.cpp touchKey()` maps them per screen via `ui::hit()`
  (rows, footer buttons, the Go to bar). Light-sleep boards (`kLightSleep`) nap between
  pages and switch off instead of deep sleep (`power.h`).
- Every firmware carries `T5READER-BOARD:<BOARD_NAME>`; Wi-Fi updates with another
  board's tag are refused. Release files are `<BOARD_NAME>-reader-<version>.bin`
  (T5-2.13: `t5-reader-…`).
- UI sizes are multiples of `S = kUiScale`; one-button boards (`kSingleButton`) map
  gestures onto the same NEXT/MENU keys (`input.h`), so app logic stays board-agnostic.
- `bb_epaper` is pinned to **2.1.11 + commit 1d92424** (font fix). When checking its API,
  read `firmware/.pio/libdeps/reader/bb_epaper/src`, not upstream `main`. Don't trust
  API names from memory or from the plan. Its built-in 6x8 font is broken in buffered
  mode: use `display::drawText6x8()`.
- Hardware-independent logic goes in `firmware/lib/core` (no Arduino includes) with a
  Unity test. UI drawing stays free of Arduino APIs so `ui-preview` can build it.
- Firmware must build without hardware (`pixi run fw-build`). Hardware-dependent
  steps end at a 🧑 checkpoint in `PLAN.md`: stop there and ask David.
- Diagnostic sketches live in `firmware/diag/<name>/main.cpp`, one PlatformIO env each.
- Host tool: Python 3.12, ruff (line length 100), pytest. Package in
  `tools/bookprep/src/bookprep`. `.t5b` spec: `docs/t5b-format.md`.
- In Python sources, write non-ASCII characters as escapes (`\N{EM DASH}` or a
  backslash-u escape), never as literal invisible characters (`normalize.py` handles
  soft hyphens, zero-width and odd spaces).
- `firmware/web/bookprep.js` is a port of the Python `bookprep`: change both together
  and run `pixi run web-test`. `bookprep/devserver.py` mirrors the firmware's web API in
  `firmware/src/transfer.cpp`: change both together. Re-embed with `pixi run web`.
- Golden image (`tests/golden/`): regenerate with `BOOKPREP_UPDATE_GOLDEN=1 pixi run test`
  only after an intended rendering change, and look at it before committing.
- UI texts: add an id with English and Dutch text to the `T5_STRINGS` table in
  `lib/core/src/i18n.h` and use `tr(S::Id)`; the web page has its own `T` table
  (en/nl) in `index.html`. Check that Dutch fits with `ui-preview` (23/24-*-nl).
- Settings: a new one goes in `lib/core/src/settings.*` (options, stable key; NVS
  stores the option index as `s_<key>`) and in devserver's `SETTINGS` table. The
  page builds its form from `/api/settings`, so it needs no change.
- Card layout: books in `/books` and one folder level, originals in `/books/.src`,
  bookmarks in `/books/.marks/<stem>.txt`, sleep pictures in `/sleep/*.bin` (one
  full-screen page each, or a square of the short side for "picture + book": told
  apart by size), fonts in `/fonts` (`storage.h`).
- The home Wi-Fi password and the AP password never go back to the page.
- The page's "Install vX" takes releases from GitHub Pages (`kUpdateSite` in
  `index.html`: `manifest-<board>.json` + the app image at 0x10000, kept on the latest
  release by `release.yml`); github.com release downloads have no CORS headers. The
  dev server's `--update-site` (tests: a folder under `<root>/www`) stands in for it.
- No secrets in the repo (`firmware/include/secrets.h` is gitignored).
- Commit per phase; tick `PLAN.md` checkboxes as work lands.

## Hardware gotchas

- V2.4 boards gate panel power with GPIO12: drive it HIGH before using the panel.
- `initIO()` sends the init sequence immediately. After `sleep(DEEP_SLEEP)` the panel
  needs a hardware reset before it accepts commands.
- Remove the SD card before flashing (GPIO2 strapping).
- GPIO39 (user button) has no internal pull-up and is input-only.
