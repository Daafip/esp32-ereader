// Board definition for the LilyGo T5-2.13 (V2.3.1 / V2.4): ESP32, SPI e-paper
// driven by bb_epaper. Included through board.h (the default board).
//
// Status column: SCHEMATIC = from LilyGo T5_2.13.pdf / README (2026-07),
// VERIFY = still to be confirmed at Checkpoint 0. See docs/hardware.md.
#pragma once

#include <stdint.h>

// Short name: release file names (<name>-reader-*.bin) and the tag that Wi-Fi
// updates check (transfer.cpp), so another board's firmware is refused.
#define BOARD_NAME "t5-213"
#include <bb_epaper.h>

#define BOARD_PANEL_BBEPAPER 1  // display_bbep.cpp

// --- E-paper panel (global SPI = VSPI, write-only) --------------------------
constexpr int kEpdMosi = 23;          // SCHEMATIC
constexpr int kEpdSck = 18;           // SCHEMATIC
constexpr int kEpdCs = 5;             // SCHEMATIC
constexpr int kEpdDc = 17;            // SCHEMATIC
constexpr int kEpdRst = 16;           // SCHEMATIC
constexpr int kEpdBusy = 4;           // SCHEMATIC
constexpr int kEpdPowerEn = 12;       // V2.4 only (NC on V2.3.1). Strapping pin: drive only after boot.
constexpr uint32_t kEpdSpiHz = 8000000;
constexpr int kEpdResetMs = 20;       // reset pulse (low)

// Panel (Checkpoint 0, 2026-10-13, panel tester): SSD1680. EP213Z_122x250's init
// gives real partial updates (~150 ms; with EP213_122x250 every partial update ran
// the 5 s full waveform). The glass shows RAM bits 8..129 of each 128-bit row, so
// the RAM window starts 1 byte in (bb_epaper's x_offset; without it the image sat
// 8 px low, bottom rows cut off).
constexpr int kEpdPanel = EP213Z_122x250;
constexpr int kEpdXOffsetBytes = 1;
// A full refresh takes ~5.2 s, longer than bb_epaper waits for BUSY (5 s).
constexpr uint32_t kEpdBusyTimeoutMs = 15000;
// Partial refresh: the OTP's "mode 2" (0xFC etc.) changes nothing on this glass
// (refresh_test round 1, 2026-10-13). A custom waveform does: Waveshare's SSD1680
// partial LUT with the 2.13" timing, 632 ms without flashing (round 2, variant B;
// display_bbep.cpp loadPartialLut). Without it, partial requests become fast ones.
constexpr bool kEpdPartialWorks = true;
constexpr bool kEpdCustomPartialLut = true;
// Fast refresh with the preparation sent before the image (display_bbep.cpp
// sendFastInit): bb_epaper's order lost the image unless the previous refresh
// was a full one (refresh_test, 2026-10-13).
constexpr bool kEpdFastInitFirst = true;
constexpr int kEpdNativeW = 122;      // bb_epaper's buffer: 16 bytes x 250 rows
constexpr int kEpdNativeH = 250;
// Landscape 250x122. 90 vs 270 (which edge is "up") is fixed in Phase 1.
constexpr int kEpdRotation = 90;          // VERIFY

// --- TF card (HSPI, separate bus per schematic) -----------------------------
// GPIO2 and GPIO15 are strapping pins; 10k pull-ups on the board.
constexpr int kSdSck = 14;            // SCHEMATIC
constexpr int kSdMiso = 2;            // SCHEMATIC
constexpr int kSdMosi = 15;           // SCHEMATIC
constexpr int kSdCs = 13;             // SCHEMATIC
constexpr uint32_t kSdSpiHz = 20000000;

// --- Buttons (active low) ---------------------------------------------------
constexpr int kBtnUser = 39;          // input-only, no internal pull-up, RTC GPIO3. Checkpoint 0
constexpr int kBtnBoot = 0;           // BOOT, RTC GPIO11: V2.4 only (not used by the reader)

// --- Battery ----------------------------------------------------------------
constexpr int kBatAdc = 35;           // ADC1_CH7, input-only. SCHEMATIC
constexpr int kBatDividerNum = 2;     // R42/R43 = 100k/100k -> Vbat = 2 x Vpin. SCHEMATIC

// --- Reader geometry: must match bookprep's "t5-213" screen / the .t5b header --
constexpr const char *kScreenId = "t5-213";
constexpr int kPageW = 250;
constexpr int kPageH = 122;
constexpr int kStatusBarH = 14;  // bottom rows, drawn on the device
constexpr int kUiScale = 1;      // menus and status bar: 1 = the 250x122 design
// Converter defaults for this screen (sent to the web page in /api/info).
constexpr int kDefaultFontPx = 13;
constexpr int kDefaultMarginPx = 4;

// --- Button roles --------------------------------------------------------------
// Only V2.4 has a BOOT button; V2.3.1 has USER and RST (Checkpoint 0, 2026-10-13:
// David's board, button test). So both use the one-button scheme on USER, as the
// T5-4.7" does (input.cpp): tap = next, double tap = previous, hold = menu /
// select, long hold = back / Settings -> Long hold. Must be an RTC GPIO (wake).
constexpr int kBtnNext = kBtnUser;
constexpr int kBtnMenu = -1;           // none: single-button scheme
constexpr int kBtnPrev = -1;           // none
constexpr bool kBtnNextPullup = false;  // GPIO39 has none; the board has an external one (idle high)

// --- Power ----------------------------------------------------------------------
constexpr bool kLightSleep = false;    // between pages: deep sleep, buttons wake (ext0/ext1)
constexpr int kPowerHoldPin = -1;      // no software power switch
constexpr int kPowerOffPulsePin = -1;
constexpr bool kSdSharedSpi = false;   // SD has its own SPI bus
