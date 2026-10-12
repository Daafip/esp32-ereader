// The T5-4.7" S3 panel (ED047TC1, 960x540, 8-bit parallel) through FastEPD.
// Kept in its own translation unit: FastEPD.h and bb_epaper.h can't be
// included together (and see tools/fastepd_symbols.py for the link side).
#pragma once

#include <stdint.h>

#include "policy.h"

namespace panel {

// Parallel bus, shift register and FastEPD's buffers (PSRAM). False on failure.
bool begin();

// 1 bpp in the panel's native landscape layout: 960x540, stride 120, MSB first,
// 1 = white. current() is the next image; previous() what the glass shows.
uint8_t *current();
uint8_t *previous();
void previousValid();  // previous() was filled in by the caller (after deep sleep)

// Full: flashing clear, then the image. Fast: drives every pixel, no flash.
// Partial: only pixels that differ from previous(). Power is off afterwards.
void update(Refresh mode);

// Panel rails off and all shift-register outputs low (LilyGo's lowest sleep current).
void powerOff();

}  // namespace panel
