// Board definition for a home-built 7.5" reader: DFRobot FireBeetle 2 ESP32-C6
// (DFR1075: 4 MB flash, no PSRAM, 16 KB RTC memory, one general-purpose SPI bus),
// Waveshare 7.5" e-Paper HAT B/W V2 (800x480, UC81xx controller, on the e-Paper
// Driver HAT Rev2.3), three DFRobot Gravity buttons (DFR0029) and, optionally, a
// microSD breakout. Included through board.h with -DBOARD_FIREBEETLE_75. See
// docs/hardware-firebeetle-75.md.
//
// Envs: reader_firebeetle75 keeps the books on the microSD card;
// reader_firebeetle75_flash (-DBOARD_BOOKS_IN_FLASH) keeps them in the flash, for a
// build without a card (room for about 43 pages, no Wi-Fi updates); fb75_test is the
// hardware test. Buttons and card are wired by hand: "Your wiring" below.
//
// Status column: SPEC = epaper-display-wiring-case-specs.md (2026-10-20), VARIANT =
// Arduino-ESP32's dfrobot_firebeetle2_esp32c6 pin variant, ADDED = not in the
// spec (the SD card), TESTED = fb75_test on David's board (2026-10-26: full refresh
// 2568 ms, fast 1495 ms, partial 986 ms + 228 ms data; BUSY floated until a loose
// connection was fixed), VERIFY = still to be checked.
#pragma once

#include <stdint.h>

// ===== Your wiring =====================================================================
// GPIO numbers, -1 = not fitted. The checks at the end of this file catch a pin used
// twice and a button that can't wake the chip.
//
// Buttons: NEXT is needed, MENU and PREV are optional.
//   NEXT, MENU, PREV  NEXT = next page (long: back), MENU = menu, PREV = back
//   NEXT, MENU        the same without PREV (the two-button scheme)
//   NEXT alone        tap = next, double tap = back, hold = menu (one-button scheme)
// Only GPIO0-7 (LP GPIOs) wake the C6 from deep sleep, so every button goes there.
constexpr int kBtnPrev = 2;           // button 1, previous (A1). SPEC
constexpr int kBtnNext = 3;           // button 2, next (A2). SPEC
constexpr int kBtnMenu = 7;           // button 3, home (D11). SPEC
// DFRobot Gravity buttons idle low (pull-down on the module) and go high when pressed.
// Plain push buttons between the pin and GND: delete this line (internal pull-ups).
#define BOARD_BUTTONS_ACTIVE_HIGH 1

// microSD card on the panel's SPI bus: card CLK to GPIO23, DI to GPIO22, and these
// two. Only the card build (env reader_firebeetle75) uses them.
constexpr int kSdMisoPin = 6;         // card DO (D12). ADDED, VERIFY
constexpr int kSdCsPin = 1;           // card CS (D6). ADDED, VERIFY
// =========================================================================================

// Short name: release file names (<name>-reader-*.bin) and the tag that Wi-Fi
// updates check (transfer.cpp), so another board's firmware is refused. The flash
// build has another partition table: it can't run in the card build's slots.
#ifdef BOARD_BOOKS_IN_FLASH
#define BOARD_NAME "firebeetle-75-flash"
#else
#define BOARD_NAME "firebeetle-75"
#endif
#include <bb_epaper.h>

#define BOARD_PANEL_BBEPAPER 1  // display_bbep.cpp
// A page (48 KB) is bigger than the C6's RTC memory: the copy of the shown image
// lives in RAM, and the app draws it again after deep sleep (display_bbep.cpp).
#define BOARD_SHOWN_IN_RAM 1
// Settings -> Orientation: portrait or landscape, either way up (board.h kScreens).
#define BOARD_TURNS 1

// --- E-paper panel: HAT switches Display Config = B (0.47R), Interface = 0 (4-wire SPI).
// HAT PWR is tied to 3V3: the panel is always powered, no power pin.
constexpr int kEpdSck = 23;           // SPEC, VARIANT (SCK)
constexpr int kEpdMosi = 22;          // SPEC, VARIANT (MOSI)
constexpr int kEpdCs = 18;            // SPEC
constexpr int kEpdDc = 14;            // SPEC
constexpr int kEpdRst = 19;           // SPEC (GPIO20 gave no output on this board)
constexpr int kEpdBusy = 21;          // SPEC: the board's MISO pin. UC81xx: low = busy. TESTED
constexpr int kEpdPowerEn = -1;       // none: HAT PWR on 3V3
constexpr uint32_t kEpdSpiHz = 10000000;  // DuPont wires. TESTED
// Reset pulse (low): the spec's ESPHome reset_duration. bb_epaper's own wake-up
// reset (10 ms, before the first command and in the partial init) works too. TESTED
constexpr int kEpdResetMs = 2;        // SPEC
// Waveshare V2 panels (bb_epaper: GEDY075-D2): full, fast and partial refresh work
// with bb_epaper's sequences. EP75_800x480 is the older GDEY075T7 init.
constexpr int kEpdPanel = EP75_800x480_GEN2;  // TESTED
constexpr int kEpdXOffsetBytes = 0;
constexpr uint32_t kEpdBusyTimeoutMs = 15000;  // a full refresh takes 2.6 s
constexpr bool kEpdPartialWorks = true;        // TESTED
constexpr bool kEpdCustomPartialLut = false;   // bb_epaper's own (T5-2.13: display_bbep.cpp)
constexpr bool kEpdFastInitFirst = false;
constexpr int kEpdNativeW = 800;
constexpr int kEpdNativeH = 480;
// Portrait like the 4.7" readers. Settings -> Orientation adds 180 and the two
// landscape rotations (board.h rotationFor()).
constexpr int kEpdRotation = 90;      // TESTED (upright)

// --- Books: a microSD card on the panel's SPI bus (the C6 has only one), or the flash.
// A 7.5" page is 48 KB, a book 10-40 MB: the flash (2 MB beside the app, no second
// app slot for Wi-Fi updates) holds about 43 pages, enough to try the reader.
#ifdef BOARD_BOOKS_IN_FLASH
constexpr int kSdMiso = -1;           // no card (storage.cpp: LittleFS on "books")
constexpr int kSdCs = -1;
#else
constexpr int kSdMiso = kSdMisoPin;
constexpr int kSdCs = kSdCsPin;
#endif
constexpr int kSdSck = kEpdSck;
constexpr int kSdMosi = kEpdMosi;
constexpr uint32_t kSdSpiHz = 20000000;
constexpr bool kSdSharedSpi = kSdCs >= 0;  // Arduino's global SPI for panel and card

// --- Buttons: the pins are in "Your wiring" at the top.
#ifdef BOARD_BUTTONS_ACTIVE_HIGH
constexpr bool kBtnNextPullup = false;  // the module's own pull-down (input.cpp adds one)
#else
constexpr bool kBtnNextPullup = true;   // plain button to GND. VERIFY: pull-ups in deep sleep
#endif

// --- Power ----------------------------------------------------------------------
constexpr bool kLightSleep = false;   // between pages: deep sleep, any button wakes (ext1)
constexpr int kPowerHoldPin = -1;     // no software power switch
constexpr int kPowerOffPulsePin = -1;

// --- Battery ----------------------------------------------------------------------
constexpr int kBatAdc = 0;            // onboard divider (DFRobot wiki). SPEC
constexpr int kBatDividerNum = 2;     // 4230 mV with a full cell. TESTED

// --- Reader geometry: must match bookprep's "7.5in" and "7.5in-landscape" screens ---
constexpr const char *kScreenId = "7.5in";
constexpr int kPageW = 480;           // portrait
constexpr int kPageH = 800;
constexpr int kStatusBarH = 28;       // bottom rows, drawn on the device
constexpr int kUiScale = 2;           // menus and status bar at twice the 250x122 design
// Converter defaults for this screen (sent to the web page in /api/info).
constexpr int kDefaultFontPx = 18;
constexpr int kDefaultMarginPx = 20;
// Landscape (800x480, Settings -> Orientation): longer lines, so a bigger default size.
constexpr const char *kTurnedScreenId = "7.5in-landscape";
constexpr int kTurnedFontPx = 22;
constexpr int kTurnedMarginPx = 32;

// --- Checks on "Your wiring" ----------------------------------------------------------
namespace fb75 {
constexpr int kPins[] = {kEpdSck, kEpdMosi, kEpdCs, kEpdDc, kEpdRst,  kEpdBusy,
                         kBatAdc, kSdMiso,  kSdCs,  kBtnPrev, kBtnNext, kBtnMenu};
constexpr bool pinsDistinct() {
  for (int i = 0; i < int(sizeof(kPins) / sizeof(kPins[0])); i++) {
    for (int j = i + 1; j < int(sizeof(kPins) / sizeof(kPins[0])); j++) {
      if (kPins[i] >= 0 && kPins[i] == kPins[j]) return false;
    }
  }
  return true;
}
constexpr bool canWake(int pin) { return pin <= 7; }  // -1 (not fitted) or an LP GPIO
}  // namespace fb75
static_assert(fb75::pinsDistinct(), "Your wiring: two things on one GPIO");
static_assert(kBtnNext >= 0, "Your wiring: the NEXT button is needed");
static_assert(fb75::canWake(kBtnNext) && fb75::canWake(kBtnMenu) && fb75::canWake(kBtnPrev),
              "Your wiring: buttons go on GPIO0-7, the only pins that wake the C6");
