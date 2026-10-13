// Board definition for the LilyGo T5-4.7" S3 (T5-ePaper-S3 V2.3/V2.4): ESP32-S3R8,
// 16 MB flash, 8 MB OPI PSRAM, ED047TC1 960x540 parallel e-paper driven by FastEPD.
// Included through board.h with -DBOARD_T5_47_S3 (env reader_t547).
//
// Status column: LILYGO = from github.com/Xinyuan-LilyGO/LilyGo-EPD47 (README GPIO
// list, src/ed047tc1.h, src/utilities.h, commit 5022422, 2026-09); VERIFY = not yet
// checked on a board. See docs/hardware-t5-47.md.
#pragma once

#include <stdint.h>

// Short name: release file names (<name>-reader-*.bin) and the tag that Wi-Fi
// updates check (transfer.cpp), so another board's firmware is refused.
#define BOARD_NAME "t5-47"

#define BOARD_PANEL_CANVAS 1   // display_canvas.cpp: portrait RAM canvas, rotated onto the panel
#define BOARD_PANEL_FASTEPD 1  // fastepd_panel.cpp

// --- E-paper panel: 8-bit parallel bus + 74HCT4094 shift register for the
// control lines (output enable, mode, STV, power rails, latch). Same wiring
// scheme as FastEPD's LilyGo T5 Pro panel, other pins. No GPIO switches the
// panel power: the rails go through the shift register.
constexpr uint8_t kEpdData[8] = {8, 1, 2, 3, 4, 5, 6, 7};  // D0..D7. LILYGO
constexpr int kEpdCkv = 38;          // LILYGO
constexpr int kEpdSth = 40;          // start pulse (SPH). LILYGO
constexpr int kEpdCkh = 41;          // pixel clock (CL). LILYGO
constexpr int kEpdCfgData = 13;      // 74HCT4094 data. LILYGO
constexpr int kEpdCfgClk = 12;       // 74HCT4094 clock. LILYGO
constexpr int kEpdCfgStr = 0;        // 74HCT4094 strobe; also the BOOT button. LILYGO
// The S3 LCD peripheral insists on a D/C pin: an unconnected GPIO (README: GPIO48
// "No Connect any").
constexpr int kEpdDummyDc = 48;      // LILYGO
constexpr uint32_t kEpdBusHz = 20000000;  // FastEPD's value for the same panel (T5 Pro)
constexpr int kEpdVcomMv = -1600;    // FastEPD's value; not programmed by this power scheme
// Native panel 960x540 landscape; the reader uses it in portrait. 90 or 270:
// which edge is "up" (USB port at the bottom or at the top).
constexpr int kEpdNativeW = 960;
constexpr int kEpdNativeH = 540;
constexpr int kEpdRotation = 90;     // VERIFY

// --- TF card (SPI) -------------------------------------------------------------
constexpr int kSdSck = 11;           // LILYGO
constexpr int kSdMiso = 16;          // LILYGO
constexpr int kSdMosi = 15;          // LILYGO
constexpr int kSdCs = 42;            // LILYGO
constexpr uint32_t kSdSpiHz = 20000000;

// --- Button (active low) ----------------------------------------------------------
// One user button. BOOT (GPIO0) doubles as the shift-register strobe, so it
// can't be read while the panel is in use.
constexpr int kBtnUser = 21;         // RTC GPIO21 (ext0 wake). LILYGO
constexpr int kBtnNext = kBtnUser;
constexpr int kBtnMenu = -1;         // none: single-button scheme (input.cpp)
constexpr int kBtnPrev = -1;         // none
constexpr bool kBtnNextPullup = true;  // external pull-up unknown: use the internal one. VERIFY

// --- Power ----------------------------------------------------------------------
constexpr bool kLightSleep = false;  // between pages: deep sleep, the button wakes (ext0)
constexpr int kPowerHoldPin = -1;    // no software power switch
constexpr int kPowerOffPulsePin = -1;
constexpr const char *kSleepHint = "Sleeping  \xC2\xB7  press the button";
constexpr bool kSdSharedSpi = false;

// --- Battery --------------------------------------------------------------------
constexpr int kBatAdc = 14;          // ADC2_CH3: read before Wi-Fi starts. LILYGO
constexpr int kBatDividerNum = 2;    // LilyGo examples: Vbat = 2 x Vpin. LILYGO

// --- Reader geometry: must match bookprep's "4.7in" screen / the .t5b header -----
constexpr const char *kScreenId = "4.7in";
constexpr int kPageW = 540;          // portrait
constexpr int kPageH = 960;
constexpr int kStatusBarH = 28;      // bottom rows, drawn on the device
constexpr int kUiScale = 2;          // menus and status bar at twice the 250x122 design
// Converter defaults for this screen (sent to the web page in /api/info).
constexpr int kDefaultFontPx = 28;
constexpr int kDefaultMarginPx = 24;
