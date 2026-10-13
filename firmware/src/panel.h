// A panel driven from a portrait RAM canvas (display_canvas.cpp). One
// implementation per kind: fastepd_panel.cpp (parallel panels: T5-4.7", PaperS3),
// it8951_panel.cpp (IT8951 controller: M5Paper). Kept apart from display.h:
// FastEPD.h and bb_epaper.h can't be included together (see tools/fastepd_symbols.py).
#pragma once

#include <stdint.h>

#include "policy.h"

namespace panel {

// Bus, controller and buffers. False on failure.
bool begin();

// 1 bpp in the panel's native landscape layout (kEpdNativeW x kEpdNativeH, MSB
// first, 1 = white): current() is the next image.
uint8_t *current();
// What the glass shows, for panels without memory (FastEPD diffs against it);
// nullptr when the controller keeps it (IT8951).
uint8_t *previous();
void previousValid();  // previous() was filled in by the caller (after deep sleep)

// Full: flashing refresh. Fast: every pixel, no flash. Partial: only changed pixels.
void update(Refresh mode);

// Between page turns in light sleep: low power, keep the image.
void standby();
// Before deep sleep or power-off.
void powerOff();

}  // namespace panel
