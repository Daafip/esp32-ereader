// Selects the board. Every pin, the panel and the screen geometry live in
// include/boards/<board>.h and nowhere else:
//   (default)          boards/t5_213.h    LilyGo T5-2.13, ESP32 + SPI e-paper
//   -DBOARD_T5_47_S3   boards/t5_47_s3.h  LilyGo T5-4.7" S3, ESP32-S3 + parallel e-paper
#pragma once

#include <stdint.h>

#if defined(BOARD_T5_47_S3)
#include "boards/t5_47_s3.h"
#else
#include "boards/t5_213.h"
#endif

// Pages: 1 bpp, MSB first, 1 = white, rows padded to whole bytes (.t5b layout).
constexpr int kPageStride = (kPageW + 7) / 8;
constexpr int kPageBytes = kPageStride * kPageH;

// One-button boards: kBtnMenu < 0. Taps, double taps and holds of the NEXT pin
// stand in for the MENU button (input.cpp).
constexpr bool kSingleButton = kBtnMenu < 0;
