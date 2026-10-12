// E-paper panel: power, refresh modes, sleep, and the landscape framebuffer.
//
// The framebuffer uses the .t5b page layout (250x122, stride 32, MSB first,
// 1 = white), so a book page is a plain memcpy. A copy of what the panel shows
// is kept in RTC memory; it survives ESP32 deep sleep and is sent as the "old"
// image for partial refreshes (see WakeRefresh in settings.h).
#pragma once

#include <bb_epaper.h>
#include <stdint.h>

#include "policy.h"

namespace display {

struct Timing {
  uint32_t dataMs = 0;     // SPI transfer
  uint32_t refreshMs = 0;  // panel update (BUSY)
};

// Powers the panel, resets it and binds bb_epaper to the framebuffer.
void begin();

// Panel sleep. keepPower: stay powered in the controller's RAM-retaining sleep
// mode with RST/CS held high through ESP32 deep sleep (WakeRefresh::Retain).
// Otherwise the controller goes to its deepest sleep and panel power is cut
// (V2.4: GPIO12 held low).
void sleep(bool keepPower);

BBEPAPER &epd();
uint8_t *framebuffer();
void clear();  // framebuffer to white

// Sends the framebuffer and refreshes. Partial falls back to Full when the
// image on the panel is unknown; Fast falls back to Full if unsupported.
// Returns the mode actually used.
Refresh show(Refresh mode);

void setRotation(int degrees);  // 90 or 270 (Phase 1 calibration)

// ASCII text in bb_epaper's built-in 6x8 font, top-left at (x, y), drawn one
// character at a time: in buffered mode bb_epaper's FONT_6x8 path draws every
// character of a string at the same x (bug in 2.1.11 and upstream main).
void drawText6x8(const char *ascii, int x, int y, int color, int bg);

bool shownValid();
uint16_t partialsSinceFull();
Timing lastTiming();

}  // namespace display
