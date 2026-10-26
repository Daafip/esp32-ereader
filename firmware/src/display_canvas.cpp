// 4.7" panels (T5-4.7" S3, M5PaperS3, M5Paper): bb_epaper draws into a portrait
// RAM canvas in the .t5b page layout; show() rotates it into the panel's native
// landscape buffer (panel.h). A parallel panel has no memory of its own and a
// page (64 KB) doesn't fit in RTC memory, so after deep sleep the shown image is
// unknown until the app redraws it (assumeShown()). Light sleep keeps it.
#include "board.h"

#ifdef BOARD_PANEL_CANVAS

#include <Arduino.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "bitmap.h"
#include "display.h"
#include "panel.h"

namespace display {
namespace {

BBEPAPER canvas;
uint8_t *fb = nullptr;  // kPageBytes, portrait
int rotation = kEpdRotation;
bool shown = false;  // panel::previous() matches the glass
RTC_DATA_ATTR uint16_t rtcPartials = 0;
Timing timing;

}  // namespace

void begin() {
  if (!fb) {
    fb = static_cast<uint8_t *>(heap_caps_malloc(kPageBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!fb) fb = static_cast<uint8_t *>(malloc(kPageBytes));
    canvas.createVirtual(kPageW, kPageH, 0);
    canvas.setBuffer(fb);
    clear();
  }
  if (!panel::begin()) Serial.println("panel: init failed");
}

void sleep(bool keepPower) {
  // Keeping the image only works while the chip keeps its RAM: light sleep.
  if (keepPower && kLightSleep) {
    panel::standby();
  } else {
    panel::powerOff();
  }
}

BBEPAPER &epd() { return canvas; }
uint8_t *framebuffer() { return fb; }
void clear() { memset(fb, 0xFF, kPageBytes); }

Refresh show(Refresh mode) {
  if (mode == Refresh::Partial && !shown) mode = Refresh::Full;
  const uint32_t t0 = millis();
  if (panel::current()) rotate1bpp(fb, kPageW, kPageH, panel::current(), rotation);
  const uint32_t t1 = millis();
  panel::update(mode);
  timing = {t1 - t0, millis() - t1};
  shown = true;
  rtcPartials = mode == Refresh::Partial ? rtcPartials + 1 : 0;
  return mode;
}

void setRotation(int degrees) { rotation = degrees; }

void setOrientation(uint8_t orientation) {
  const int r = rotationFor(orientation);
  if (r == rotation) return;
  rotation = r;
  shown = false;  // the next partial update becomes a full one
}

int screenIndex() { return 0; }  // one screen, either way up
const ScreenSpec &screen() { return kScreens[0]; }

void drawText6x8(const char *ascii, int x, int y, int color, int bg) {
  canvas.setFont(FONT_6x8);
  canvas.setTextColor(color, bg);
  char one[2] = {0, 0};
  for (; *ascii && x + 6 <= canvas.width(); ascii++, x += 6) {
    one[0] = (*ascii >= 32 && *ascii < 127) ? *ascii : '?';
    canvas.drawString(one, x, y);
  }
}

void assumeShown() {
  if (panel::previous()) {
    rotate1bpp(fb, kPageW, kPageH, panel::previous(), rotation);
    panel::previousValid();
  }
  shown = true;
}

bool shownValid() { return shown; }
uint16_t partialsSinceFull() { return rtcPartials; }
Timing lastTiming() { return timing; }

}  // namespace display

#endif  // BOARD_PANEL_CANVAS
