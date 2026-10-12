// Framebuffer-only stand-in for the display backends (UI previews on the PC).
// Same canvas as on the board: the rotated panel buffer (T5-2.13) or a virtual
// portrait canvas (T5-4.7" S3).
#include <string.h>

#include "board.h"
#include "display.h"

namespace display {
namespace {
#ifdef BOARD_PANEL_BBEPAPER
BBEPAPER panel(kEpdPanel);
uint8_t fb[((122 + 7) / 8) * 250];
#else
BBEPAPER panel;
uint8_t fb[kPageBytes];
#endif
}  // namespace

void begin() {
#ifdef BOARD_PANEL_BBEPAPER
  panel.setRotation(kEpdRotation);
#else
  panel.createVirtual(kPageW, kPageH, 0);
#endif
  panel.setBuffer(fb);
}
void sleep(bool) {}
BBEPAPER &epd() { return panel; }
uint8_t *framebuffer() { return fb; }
void clear() { memset(fb, 0xFF, sizeof(fb)); }
Refresh show(Refresh mode) { return mode; }
void setRotation(int degrees) { panel.setRotation(degrees); }
void drawText6x8(const char *ascii, int x, int y, int color, int bg) {
  panel.setFont(FONT_6x8);
  panel.setTextColor(color, bg);
  char one[2] = {0, 0};
  for (; *ascii && x + 6 <= panel.width(); ascii++, x += 6) {
    one[0] = (*ascii >= 32 && *ascii < 127) ? *ascii : '?';
    panel.drawString(one, x, y);
  }
}
void assumeShown() {}
bool shownValid() { return true; }
uint16_t partialsSinceFull() { return 0; }
Timing lastTiming() { return {}; }
}  // namespace display
