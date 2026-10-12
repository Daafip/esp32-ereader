# T5-2.13 pocket e-reader

ESP32 e-reader on a LilyGo T5-2.13 (SSD1680-class 122×250 SPI e-paper, landscape
250×122). The host pre-renders books to 1-bpp page bitmaps (`.t5b`); the device only
blits pages. Full plan and phase status: `PLAN.md`. Hardware facts: `docs/hardware.md`.
Second build target (Phase 7, example port): LilyGo T5-4.7" S3, ESP32-S3 + 960×540
parallel panel via FastEPD, used portrait 540×960, one button (`docs/hardware-t5-47.md`).

## Current phase

Phases 0-5 are written and build; **no code has run on a board yet** (David has none
yet). Waiting on the hardware checkpoints in `docs/checkpoints.md` (0, 1, 3, 4).
Checkpoint 2 is done: Ubuntu Regular 13 px (bookprep default). Phase 6 is optional and
not started except Wi-Fi transfer (done). UI look: check `pixi run ui-preview` after
any change to `ui.cpp` (both boards; T5-2.13 screens should stay pixel-identical unless
intended); the web page opens in demo mode straight from disk (`?screen=t5-47`).

## Commands (everything through pixi)

| Task | Command |
|---|---|
| Build all firmware envs | `pixi run fw-build` |
| Firmware unit tests (PC) | `pixi run fw-test` (`firmware/lib/core`, Unity, native env) |
| Render all screens (PC) | `pixi run ui-preview` -> `out/ui-preview/<screen>/` (`t5-213`, `t5-47`) |
| Flash one env | `pixi run fw-flash reader` (`calibration`, `panel_tester`, `button_test`, `sd_test`; T5-4.7": `reader_t547`) |
| Images for browser/OTA flashing | `pixi run fw-dist` -> `out/firmware/` |
| Serial monitor | `pixi run fw-monitor` |
| Convert a book | `pixi run prep book.epub --font X.ttf --out out/ --preview` (`--screen t5-47`) |
| Font comparison | `pixi run font-sheet book.epub --font a.ttf --sizes 11 12 13` |
| Regenerate generated headers | `pixi run devpages` (test_pages.h), `pixi run ui-fonts` (fonts/), `pixi run web` (web_index.h from `firmware/web/index.html`) |
| Host tests / lint | `pixi run test`, `pixi run lint`, `pixi run fmt` |
| Browser converter tests | `pixi run web-test` (Node parity + headless Chromium) |
| Web page without the reader | `pixi run web-dev` (`--lan` for a phone, `--screen t5-47`) |

## Conventions

- All pins, the panel and the screen geometry live in `firmware/include/boards/<board>.h`
  (selected by `board.h`), nowhere else. A screen's size and converter defaults also
  appear in `SCREENS` (`bookprep/layout.py`) and `screens` (`firmware/web/bookprep.js`):
  change all three together.
- Display backends: `display_bbep.cpp` (SPI panels) and `display_fastepd.cpp` +
  `fastepd_panel.cpp` (T5-4.7"). FastEPD.h and bb_epaper.h never go in one translation
  unit; `firmware/tools/fastepd_symbols.py` renames their duplicate symbols at link.
  FastEPD is pinned to 2.2.0: read `firmware/.pio/libdeps/reader_t547/FastEPD/src`.
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
- No secrets in the repo (`firmware/include/secrets.h` is gitignored).
- Commit per phase; tick `PLAN.md` checkboxes as work lands.

## Hardware gotchas

- V2.4 boards gate panel power with GPIO12: drive it HIGH before using the panel.
- `initIO()` sends the init sequence immediately. After `sleep(DEEP_SLEEP)` the panel
  needs a hardware reset before it accepts commands.
- Remove the SD card before flashing (GPIO2 strapping).
- GPIO39 (user button) has no internal pull-up and is input-only.
