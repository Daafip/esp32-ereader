// SPI e-paper (T5-2.13) through bb_epaper: the panel's own controller keeps the
// image, a copy in RTC memory serves partial refreshes after deep sleep.
#include "board.h"

#ifdef BOARD_PANEL_BBEPAPER

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_attr.h>
#include <string.h>

#include "display.h"

namespace display {
namespace {

BBEPAPER panel(kEpdPanel);
// bb_epaper sizes its buffer by the native panel (16 x 250 = 4000 B); the
// landscape page only uses the first kPageBytes of it.
uint8_t fb[((122 + 7) / 8) * 250];
static_assert(sizeof(fb) >= kPageBytes, "framebuffer too small for a page");

// Survives deep sleep; zeroed on power-on.
RTC_DATA_ATTR uint8_t rtcShown[kPageBytes];
RTC_DATA_ATTR bool rtcShownValid = false;
RTC_DATA_ATTR bool rtcKeptPowered = false;
RTC_DATA_ATTR uint16_t rtcPartials = 0;

bool panelRamValid = false;  // controller's "old" RAM equals rtcShown
Timing timing;

const gpio_num_t kHeldPins[] = {gpio_num_t(kEpdPowerEn), gpio_num_t(kEpdRst), gpio_num_t(kEpdCs)};

void releaseHolds() {
  gpio_deep_sleep_hold_dis();
  for (gpio_num_t pin : kHeldPins) gpio_hold_dis(pin);
}

// Keeps pin levels through ESP32 deep sleep. With the panel powered, RST and CS
// stay high (no reset, no stray commands). With power cut, only the power pin
// is held (low): driving RST/CS high would back-power the panel.
void holdPins(bool panelPowered) {
  if (panelPowered) {
    for (gpio_num_t pin : kHeldPins) gpio_hold_en(pin);
  } else {
    gpio_hold_en(gpio_num_t(kEpdPowerEn));
  }
  gpio_deep_sleep_hold_en();
}

void hardwareReset() {
  pinMode(kEpdCs, OUTPUT);
  digitalWrite(kEpdCs, HIGH);
  pinMode(kEpdBusy, INPUT);
  pinMode(kEpdRst, OUTPUT);
  digitalWrite(kEpdRst, LOW);
  delay(20);
  digitalWrite(kEpdRst, HIGH);
  delay(20);
}

}  // namespace

void begin() {
  releaseHolds();
  pinMode(kEpdPowerEn, OUTPUT);
  digitalWrite(kEpdPowerEn, HIGH);
  if (!rtcKeptPowered) delay(10);  // let the panel supply settle
  hardwareReset();                  // also leaves the controller's deep sleep
  panel.initIO(kEpdDc, kEpdRst, kEpdBusy, kEpdCs, kEpdMosi, kEpdSck, kEpdSpiHz);
  panel.setRotation(kEpdRotation);
  panel.setBuffer(fb);
  panelRamValid = rtcKeptPowered && rtcShownValid;
  rtcKeptPowered = false;
}

void sleep(bool keepPower) {
  panel.sleep(keepPower ? LIGHT_SLEEP : DEEP_SLEEP);  // SSD16xx: 0x10 = 0x01 keeps RAM, 0x03 not
  rtcKeptPowered = keepPower;
  if (!keepPower) digitalWrite(kEpdPowerEn, LOW);
  holdPins(keepPower);
}

BBEPAPER &epd() { return panel; }
uint8_t *framebuffer() { return fb; }
void clear() { memset(fb, 0xFF, sizeof(fb)); }

Refresh show(Refresh mode) {
  if (mode == Refresh::Partial && !rtcShownValid) mode = Refresh::Full;
  if (mode == Refresh::Fast && !panel.hasFastRefresh()) mode = Refresh::Full;

  const uint32_t t0 = millis();
  if (mode == Refresh::Partial) {
    if (!panelRamValid) {  // tell the controller what is on the glass now
      panel.setBuffer(rtcShown);
      panel.writePlane(PLANE_0_TO_1);
      panel.setBuffer(fb);
    }
    panel.writePlane(PLANE_0);
  } else {
    panel.writePlane(PLANE_DUPLICATE);
  }
  const uint32_t t1 = millis();
  panel.refresh(mode == Refresh::Full   ? REFRESH_FULL
                : mode == Refresh::Fast ? REFRESH_FAST
                                        : REFRESH_PARTIAL,
                true);
  const uint32_t t2 = millis();
  if (mode == Refresh::Partial) {
    // SSD16xx may not copy new -> old RAM after a partial update; do it, so
    // the next partial update can skip resending the old image.
    panel.writePlane(PLANE_0_TO_1);
  }
  timing = {t1 - t0, t2 - t1};

  memcpy(rtcShown, fb, kPageBytes);
  rtcShownValid = true;
  panelRamValid = true;
  rtcPartials = mode == Refresh::Partial ? rtcPartials + 1 : 0;
  return mode;
}

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

void assumeShown() {
  memcpy(rtcShown, fb, kPageBytes);
  rtcShownValid = true;
  panelRamValid = false;
}

bool shownValid() { return rtcShownValid; }
uint16_t partialsSinceFull() { return rtcPartials; }
Timing lastTiming() { return timing; }

}  // namespace display

#endif  // BOARD_PANEL_BBEPAPER
