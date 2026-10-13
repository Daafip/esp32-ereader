// Board definition for the M5Stack M5Paper (the original, ESP32): ESP32-D0WDQ6-V3,
// 16 MB flash, 8 MB PSRAM, IT8951 e-paper controller + ED047TC1 960x540 on SPI,
// GT911 touch, a 3-way switch (up / press / down).
// Included through board.h with -DBOARD_M5PAPER (env reader_m5paper).
//
// Status column: M5 = from M5Unified / M5GFX (github.com/m5stack, 2026-10-13:
// M5Unified.inl pin tables, Power_Class.inl, M5GFX.cpp autodetect); VERIFY = not
// yet checked on a board. See docs/hardware-m5paper.md.
#pragma once

#include <stdint.h>

// Short name: release file names (<name>-reader-*.bin) and the tag that Wi-Fi
// updates check (transfer.cpp), so another board's firmware is refused.
#define BOARD_NAME "m5paper"

#define BOARD_PANEL_CANVAS 1    // display_canvas.cpp: portrait RAM canvas, rotated onto the panel
#define BOARD_PANEL_IT8951 1    // it8951_panel.cpp
#define BOARD_HAS_TOUCH 1

// --- IT8951 on SPI (VSPI, shared with the SD card) --------------------------------
constexpr int kSpiSck = 14;              // M5
constexpr int kSpiMosi = 12;             // M5
constexpr int kSpiMiso = 13;             // M5
constexpr int kEpdCs = 15;               // M5
constexpr int kEpdBusy = 27;             // HRDY: high = ready for the next word. M5
constexpr int kEpdPowerEn = 23;          // IT8951 power (M5GFX drives it as reset). M5
constexpr uint32_t kEpdSpiHz = 20000000; // M5GFX: 40 MHz writes, 20 MHz reads; 12/2 MHz at init
constexpr int kEpdNativeW = 960;
constexpr int kEpdNativeH = 540;
constexpr int kEpdRotation = 270;        // M5GFX uses offset_rotation 3 for portrait. VERIFY

// --- TF card: same SPI bus as the IT8951 ----------------------------------------
constexpr int kSdSck = kSpiSck;
constexpr int kSdMosi = kSpiMosi;
constexpr int kSdMiso = kSpiMiso;
constexpr int kSdCs = 4;                 // M5
constexpr uint32_t kSdSpiHz = 20000000;
constexpr bool kSdSharedSpi = true;      // both use Arduino's global SPI object

// --- Touch: GT911 -------------------------------------------------------------------
constexpr int kTouchSda = 21;            // M5
constexpr int kTouchScl = 22;            // M5
constexpr int kTouchInt = 36;            // M5 (RTC GPIO, input-only)
constexpr int kTouchRawW = 540;          // M5GFX: x 0..539, y 0..959
constexpr int kTouchRawH = 960;
constexpr bool kTouchFlip = false;       // rotate touch 180 degrees (if kEpdRotation is flipped). VERIFY

// --- Buttons: the 3-way switch, active low, external pull-ups (input-only pins) ---
constexpr int kBtnNext = 39;             // M5Unified BtnC. VERIFY which end is "down"
constexpr int kBtnMenu = 38;             // press (BtnB). M5
constexpr int kBtnPrev = 37;             // BtnA. VERIFY
constexpr bool kBtnNextPullup = false;

// --- Power ----------------------------------------------------------------------
// Light sleep between pages (the IT8951 keeps the image in standby); "sleep"
// switches the device off: on battery, GPIO2 (main power hold) low cuts the power.
constexpr bool kLightSleep = true;
constexpr int kPowerHoldPin = 2;         // high = stay on. M5
constexpr int kPowerOffPulsePin = -1;
constexpr const char *kSleepHint = "Off  \xC2\xB7  press the switch to read on";
constexpr int kBatAdc = 35;              // ADC1_CH7. M5
constexpr int kBatDividerNum = 2;        // M5Unified: ratio 2.0. M5

// --- Reader geometry: must match bookprep's "4.7in" screen / the .t5b header -----
constexpr const char *kScreenId = "4.7in";
constexpr int kPageW = 540;              // portrait
constexpr int kPageH = 960;
constexpr int kStatusBarH = 28;          // bottom rows, drawn on the device
constexpr int kUiScale = 2;              // menus and status bar at twice the 250x122 design
// Converter defaults for this screen (sent to the web page in /api/info).
constexpr int kDefaultFontPx = 28;
constexpr int kDefaultMarginPx = 24;
