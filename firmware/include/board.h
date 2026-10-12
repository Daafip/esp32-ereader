// Board definition for the LilyGo T5-2.13 (V2.3.1 / V2.4).
// Every pin and the panel constant live here and nowhere else.
//
// Status column: SCHEMATIC = from LilyGo T5_2.13.pdf / README (2026-07),
// VERIFY = still to be confirmed at Checkpoint 0. See docs/hardware.md.
#pragma once

#include <stdint.h>
#include <bb_epaper.h>

// --- E-paper panel (global SPI = VSPI, write-only) --------------------------
constexpr int kEpdMosi = 23;          // SCHEMATIC
constexpr int kEpdSck = 18;           // SCHEMATIC
constexpr int kEpdCs = 5;             // SCHEMATIC
constexpr int kEpdDc = 17;            // SCHEMATIC
constexpr int kEpdRst = 16;           // SCHEMATIC
constexpr int kEpdBusy = 4;           // SCHEMATIC
constexpr int kEpdPowerEn = 12;       // V2.4 only (NC on V2.3.1). Strapping pin: drive only after boot.
constexpr uint32_t kEpdSpiHz = 8000000;

// Panel constant: provisional until Checkpoint 0 (panel tester decides).
constexpr int kEpdPanel = EP213_122x250;  // VERIFY
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
constexpr int kBtnUser = 39;          // input-only, no internal pull-up, RTC GPIO3. VERIFY
constexpr int kBtnBoot = 0;           // BOOT, RTC GPIO11. VERIFY

// --- Battery ----------------------------------------------------------------
constexpr int kBatAdc = 35;           // ADC1_CH7, input-only. SCHEMATIC
constexpr int kBatDividerNum = 2;     // R42/R43 = 100k/100k -> Vbat = 2 x Vpin. SCHEMATIC

// --- Reader geometry: must match bookprep's PageGeometry / the .t5b header --
constexpr int kPageW = 250;
constexpr int kPageH = 122;
constexpr int kPageStride = (kPageW + 7) / 8;     // 32
constexpr int kPageBytes = kPageStride * kPageH;  // 3904
constexpr int kStatusBarH = 10;                   // bottom rows, drawn on the device

// --- Button roles --------------------------------------------------------------
// NEXT: short = next page / next item, long = previous page / previous item.
// MENU: short = menu / select, long = sleep / back.
// Both must be RTC GPIOs (deep-sleep wake). Remap here for the 4-button variant.
constexpr int kBtnNext = kBtnUser;
constexpr int kBtnMenu = kBtnBoot;
