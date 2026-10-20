// Selects the board. Every pin, the panel and the screen geometry live in
// include/boards/<board>.h and nowhere else:
//   (default)            boards/t5_213.h      LilyGo T5-2.13, ESP32 + SPI e-paper
//   -DBOARD_T5_47_S3     boards/t5_47_s3.h    LilyGo T5-4.7" S3, ESP32-S3 + parallel e-paper
//   -DBOARD_M5PAPER_S3   boards/m5paper_s3.h  M5Stack M5PaperS3, ESP32-S3 + parallel e-paper, touch
//   -DBOARD_M5PAPER      boards/m5paper.h     M5Stack M5Paper, ESP32 + IT8951 controller, touch
#pragma once

#include <stdint.h>

#if defined(BOARD_T5_47_S3)
#include "boards/t5_47_s3.h"
#elif defined(BOARD_M5PAPER_S3)
#include "boards/m5paper_s3.h"
#elif defined(BOARD_M5PAPER)
#include "boards/m5paper.h"
#else
#include "boards/t5_213.h"
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

// One-button boards: a NEXT pin but no MENU pin. Taps, double taps and holds of
// the NEXT pin stand in for the MENU button (input.cpp). Touch-only boards have
// neither (kBtnNext < 0).
constexpr bool kSingleButton = kBtnNext >= 0 && kBtnMenu < 0;
