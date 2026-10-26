// E-paper panel: power, refresh modes, sleep, and the framebuffer.
//
// The framebuffer uses the .t5b page layout of the current screen (screen().w x
// screen().h, MSB first, 1 = white), so a book page is a plain memcpy, and
// bb_epaper draws the UI into it on every board. Backends: display_bbep.cpp (SPI
// panels with their own controller RAM: T5-2.13, 7.5") and display_canvas.cpp
// (portrait RAM canvas for the 4.7" panels).
//
// T5-2.13: a copy of what the panel shows is kept in RTC memory; it survives
// deep sleep and is sent as the "old" image for partial refreshes (see
// WakeRefresh in settings.h). 4.7" and 7.5": a page is too big for RTC memory, so
// after deep sleep shownValid() is false until the app redraws the old image
// and calls assumeShown().
#pragma once

#include <bb_epaper.h>
#include <stdint.h>

#include "board.h"
#include "policy.h"

namespace display {

struct Timing {
  uint32_t dataMs = 0;     // SPI transfer
  uint32_t refreshMs = 0;  // panel update (BUSY)
};

// Powers the panel, resets it and binds bb_epaper to the framebuffer.
void begin();

// Panel sleep. keepPower: stay powered in the controller's RAM-retaining sleep
// mode with RST/CS held high through ESP32 deep sleep (WakeRefresh::Retain;
// SPI panels only). Otherwise the controller goes to its deepest sleep and panel
// power is cut (T5-2.13 V2.4: GPIO12 held low).
void sleep(bool keepPower);

BBEPAPER &epd();
uint8_t *framebuffer();
void clear();  // framebuffer to white

// Sends the framebuffer and refreshes. Partial falls back to Full when the
// image on the panel is unknown; Fast falls back to Full if unsupported.
// Returns the mode actually used.
Refresh show(Refresh mode);

void setRotation(int degrees);  // 90 or 270 (calibration)

// Settings -> Flip screen / Orientation (settings.h): 0 as built, 1 upside down;
// boards that turn (BOARD_TURNS) also 2 and 3, the other screen (board.h
// rotationFor()). A change makes the next show() a full refresh: the glass has
// the old orientation.
void setOrientation(uint8_t orientation);

// The screen the reader runs at now (board.h kScreens), and its index: books and
// full-screen pictures must be made for it.
const ScreenSpec &screen();
int screenIndex();

// ASCII text in bb_epaper's built-in 6x8 font, top-left at (x, y), drawn one
// character at a time: in buffered mode bb_epaper's FONT_6x8 path draws every
// character of a string at the same x (bug in 2.1.11 and upstream main).
void drawText6x8(const char *ascii, int x, int y, int color, int bg);

// The framebuffer holds what the panel shows right now (redrawn by the app);
// the next show(Partial) diffs against it.
void assumeShown();

bool shownValid();
uint16_t partialsSinceFull();
Timing lastTiming();

}  // namespace display
