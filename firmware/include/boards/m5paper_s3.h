// Board definition for the M5Stack M5PaperS3: ESP32-S3R8, 16 MB flash, 8 MB OPI
// PSRAM, ED047TC1 960x540 parallel e-paper (FastEPD preset BB_PANEL_M5PAPERS3),
// GT911 touch, no user buttons (the power button only switches the power).
// Included through board.h with -DBOARD_M5PAPER_S3 (env reader_m5papers3).
//
// Status column: M5 = from M5Unified / M5GFX (github.com/m5stack, 2026-10-13:
// M5Unified.inl pin tables, Power_Class.inl, M5GFX.cpp autodetect) and FastEPD's
// preset; VERIFY = not yet checked on a board. See docs/hardware-m5paper.md.
#pragma once

#include <stdint.h>

// Short name: release file names (<name>-reader-*.bin) and the tag that Wi-Fi
// updates check (transfer.cpp), so another board's firmware is refused.
#define BOARD_NAME "m5papers3"

#define BOARD_PANEL_CANVAS 1    // display_canvas.cpp: portrait RAM canvas, rotated onto the panel
#define BOARD_PANEL_FASTEPD 1   // fastepd_panel.cpp
#define BOARD_FASTEPD_PRESET BB_PANEL_M5PAPERS3
#define BOARD_HAS_TOUCH 1

// --- E-paper panel: pins come with FastEPD's preset (same as M5GFX's Bus_EPD):
// D0..D7 = 6, 14, 7, 12, 9, 11, 8, 10; PWR 46, SPV 17, CKV 18, SPH 13, OE 45,
// LE 15, CL 16. The preset's dummy D/C pin is GPIO47, which is also the SD card's
// CS: fastepd_panel.cpp hands GPIO47 back to the GPIO matrix after the bus init.
constexpr int kEpdDummyDc = 47;          // M5 (FastEPD preset)
constexpr int kEpdNativeW = 960;
constexpr int kEpdNativeH = 540;
constexpr int kEpdRotation = 270;        // M5GFX uses offset_rotation 3 for portrait. VERIFY

// --- TF card (SPI) -----------------------------------------------------------------
constexpr int kSdSck = 39;               // M5
constexpr int kSdMosi = 38;              // M5
constexpr int kSdMiso = 40;              // M5
constexpr int kSdCs = 47;                // M5
constexpr uint32_t kSdSpiHz = 20000000;
constexpr bool kSdSharedSpi = false;

// --- Touch: GT911 on the internal I2C bus ---------------------------------------
constexpr int kTouchSda = 41;            // M5
constexpr int kTouchScl = 42;            // M5
constexpr int kTouchInt = 48;            // M5. Not an RTC GPIO: wakes light sleep only
constexpr int kTouchRawW = 540;          // M5GFX: x 0..539, y 0..959
constexpr int kTouchRawH = 960;
constexpr bool kTouchFlip = false;       // rotate touch 180 degrees (if kEpdRotation is flipped). VERIFY

// --- RTC: BM8563 on the touch I2C bus, keeps the clock while switched off ---------
#define BOARD_HAS_RTC 1
constexpr uint8_t kRtcAddress = 0x51;    // M5

// --- Buttons: none ---------------------------------------------------------------
constexpr int kBtnNext = -1;
constexpr int kBtnMenu = -1;
constexpr int kBtnPrev = -1;
constexpr bool kBtnNextPullup = false;

// --- Power ----------------------------------------------------------------------
// The touch INT can't wake deep sleep: light sleep between pages (RAM, panel
// buffers and touch stay alive), and "sleep" switches the device off. Pulses on
// PWROFF_PULSE (GPIO44) cut the power; the power button turns it back on.
constexpr bool kLightSleep = true;
constexpr int kPowerHoldPin = -1;
constexpr int kPowerOffPulsePin = 44;    // M5 (kept low while running)
constexpr int kBatAdc = 3;               // ADC1_CH2. M5
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
