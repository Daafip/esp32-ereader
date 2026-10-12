# T5-2.13 pocket e-reader

ESP32 e-reader on a LilyGo T5-2.13 (SSD1680-class 122×250 SPI e-paper, landscape
250×122). The host pre-renders books to 1-bpp page bitmaps (`.t5b`); the device only
blits pages. Full plan and phase status: `PLAN.md`. Hardware facts: `docs/hardware.md`.

## Current phase

**Phase 0 code done; waiting on HUMAN CHECKPOINT 0** (panel tester, button test and
SD test run on the board). Phase 2 (host tool) doesn't need the board.

## Commands (everything through pixi)

| Task | Command |
|---|---|
| Build all firmware envs | `pixi run fw-build` |
| Flash one env | `pixi run fw-flash panel_tester` (`reader`, `button_test`, `sd_test`) |
| Serial monitor | `pixi run fw-monitor` |
| Convert a book | `pixi run prep book.epub --font tools/bookprep/fonts/X.ttf --out out/` |
| Host tests / lint | `pixi run test`, `pixi run lint`, `pixi run fmt` |

## Conventions

- All pins and the panel constant live in `firmware/include/board.h`, nowhere else.
- `bb_epaper` is pinned to **2.1.11**. When checking its API, read the source of *that
  tag* (`main` has newer panel enums that don't compile against 2.1.11). Don't trust
  API names from memory or from the plan.
- Firmware must build without hardware (`pixi run fw-build`). Hardware-dependent
  steps end at a 🧑 checkpoint in `PLAN.md`: stop there and ask David.
- Diagnostic sketches live in `firmware/diag/<name>/main.cpp`, one PlatformIO env each.
- Host tool: Python 3.12, ruff (line length 100), pytest. Package in
  `tools/bookprep/src/bookprep`.
- No secrets in the repo (`firmware/include/secrets.h` is gitignored).
- Commit per phase; tick `PLAN.md` checkboxes as work lands.

## Hardware gotchas

- V2.4 boards gate panel power with GPIO12: drive it HIGH before using the panel.
- `initIO()` sends the init sequence immediately. After `sleep(DEEP_SLEEP)` the panel
  needs a hardware reset before it accepts commands.
- Remove the SD card before flashing (GPIO2 strapping).
- GPIO39 (user button) has no internal pull-up and is input-only.
