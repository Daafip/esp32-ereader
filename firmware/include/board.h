// Selects the board. Every pin, the panel and the screen geometry live in
// include/boards/<board>.h and nowhere else:
//   (default)            boards/t5_213.h      LilyGo T5-2.13, ESP32 + SPI e-paper
//   -DBOARD_T5_47_S3     boards/t5_47_s3.h    LilyGo T5-4.7" S3, ESP32-S3 + parallel e-paper
//   -DBOARD_M5PAPER_S3   boards/m5paper_s3.h  M5Stack M5PaperS3, ESP32-S3 + parallel e-paper, touch
//   -DBOARD_M5PAPER      boards/m5paper.h     M5Stack M5Paper, ESP32 + IT8951 controller, touch
//   -DBOARD_FIREBEETLE_75 boards/firebeetle_75.h FireBeetle 2 ESP32-C6 + Waveshare 7.5" SPI e-paper
#pragma once

#include <stdint.h>

#if defined(BOARD_T5_47_S3)
#include "boards/t5_47_s3.h"
#elif defined(BOARD_M5PAPER_S3)
#include "boards/m5paper_s3.h"
#elif defined(BOARD_M5PAPER)
#include "boards/m5paper.h"
#elif defined(BOARD_FIREBEETLE_75)
#include "boards/firebeetle_75.h"
#else
#include "boards/t5_213.h"
#endif

// Buttons: active low (pull-ups) on every board but the FireBeetle's Gravity modules.
#ifdef BOARD_BUTTONS_ACTIVE_HIGH
constexpr bool kBtnActiveHigh = true;
#else
constexpr bool kBtnActiveHigh = false;
#endif

// Touch screen (GT911): kTouch* pins in the board header (touch_gt911.cpp).
#ifdef BOARD_HAS_TOUCH
constexpr bool kHasTouch = true;
#else
constexpr bool kHasTouch = false;
#endif

// Pages: 1 bpp, MSB first, 1 = white, rows padded to whole bytes (.t5b layout).
constexpr int kPageStride = (kPageW + 7) / 8;
constexpr int kPageBytes = kPageStride * kPageH;
// Square sleep pictures ("picture + book"): the screen's short side, same layout.
constexpr int kSquareSide = kPageW < kPageH ? kPageW : kPageH;
constexpr int kSquareStride = (kSquareSide + 7) / 8;
constexpr int kSquareBytes = kSquareStride * kSquareSide;

// A page geometry the reader runs at, with the converter defaults for it: must
// match bookprep's SCREENS (layout.py) and screens (bookprep.js).
struct ScreenSpec {
  const char *id;
  int w, h;
  int statusBar;         // bottom rows, drawn on the device
  int fontPx, marginPx;  // converter defaults (sent to the web page in /api/info)
};

// The board's screens: kPageW x kPageH, and on boards that turn (BOARD_TURNS: the
// 7.5") a second one a quarter turn away, for Settings -> Orientation. A book or a
// full-screen picture is made for one of them; kPageBytes holds a page of either.
#ifdef BOARD_TURNS
constexpr ScreenSpec kScreens[] = {
    {kScreenId, kPageW, kPageH, kStatusBarH, kDefaultFontPx, kDefaultMarginPx},
    {kTurnedScreenId, kPageH, kPageW, kStatusBarH, kTurnedFontPx, kTurnedMarginPx},
};
static_assert((kPageH + 7) / 8 * kPageW == kPageBytes, "a turned page must be as big as a page");
#else
constexpr ScreenSpec kScreens[] = {
    {kScreenId, kPageW, kPageH, kStatusBarH, kDefaultFontPx, kDefaultMarginPx},
};
#endif
constexpr int kScreenCount = int(sizeof(kScreens) / sizeof(kScreens[0]));

// Panel rotation for an orientation (settings.h): 0 as built, 1 upside down, and on
// boards that turn 2 and 3, a quarter turn more (elsewhere the same as 0 and 1).
constexpr int rotationFor(int orientation) {
  return (kEpdRotation + ((orientation & 1) ? 180 : 0) +
          (kScreenCount > 1 && (orientation & 2) ? 270 : 0)) %
         360;
}
// The screen (index into kScreens) a panel rotation shows.
constexpr int screenForRotation(int rotation) {
  return (rotation - kEpdRotation + 360) % 180 ? 1 : 0;
}

// One-button boards: a NEXT pin but no MENU pin. Taps, double taps and holds of
// the NEXT pin stand in for the MENU button (input.cpp). Touch-only boards have
// neither (kBtnNext < 0).
constexpr bool kSingleButton = kBtnNext >= 0 && kBtnMenu < 0;
