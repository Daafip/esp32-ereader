// Framebuffer-only stand-in for src/display.cpp (UI previews on the PC).
#include <string.h>

#include "board.h"
#include "display.h"

namespace display {
namespace {
BBEPAPER panel(kEpdPanel);
uint8_t fb[((122 + 7) / 8) * 250];
}  // namespace

void begin() {
  panel.setRotation(kEpdRotation);
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
bool shownValid() { return true; }
uint16_t partialsSinceFull() { return 0; }
Timing lastTiming() { return {}; }
}  // namespace display
