// Button test for the T5-2.13.
//
// Logs every press/release on the user button (GPIO39) and BOOT (GPIO0) with
// millisecond timestamps and press duration, so we can confirm the GPIOs,
// their idle level, bounce, and pick a long-press threshold.
//
// Also watches a few free header GPIOs (with internal pull-ups) in case this
// is a variant with extra buttons; any edge there is logged as "extra".
#include <Arduino.h>

#include "board.h"

namespace {

struct Watched {
  int pin;
  const char *name;
  bool pullup;  // GPIO34-39 have no internal pull-ups
  int stable;
  int lastRaw;
  uint32_t lastChangeMs;
  uint32_t pressedAtMs;
  uint32_t bounces;
};

Watched watched[] = {
    {kBtnUser, "USER(39)", false},
    {kBtnBoot, "BOOT(0)", true},
    {32, "extra(32)", true},
    {33, "extra(33)", true},
    {25, "extra(25)", true},
    {26, "extra(26)", true},
    {27, "extra(27)", true},
};
constexpr uint32_t kDebounceMs = 25;
constexpr uint32_t kLongPressMs = 600;

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nT5-2.13 button test. Press each button short, long, and rapidly.");
  Serial.printf("debounce %u ms, long press >= %u ms\n", (unsigned)kDebounceMs,
                (unsigned)kLongPressMs);
  for (auto &w : watched) {
    pinMode(w.pin, w.pullup ? INPUT_PULLUP : INPUT);
  }
  delay(5);
  for (auto &w : watched) {
    w.stable = w.lastRaw = digitalRead(w.pin);
    Serial.printf("%-10s GPIO%-2d idle level %d%s\n", w.name, w.pin, w.stable,
                  w.stable ? "" : "  <-- LOW at idle: not a pulled-up button?");
  }
  Serial.println("t_ms       button      event     duration  bounces");
}

void loop() {
  const uint32_t now = millis();
  for (auto &w : watched) {
    const int raw = digitalRead(w.pin);
    if (raw != w.lastRaw) {
      w.lastRaw = raw;
      w.lastChangeMs = now;
      w.bounces++;
    }
    if (raw == w.stable || now - w.lastChangeMs < kDebounceMs) continue;
    w.stable = raw;
    if (raw == LOW) {
      w.pressedAtMs = now;
      Serial.printf("%-10u %-11s press               %u\n", (unsigned)now, w.name,
                    (unsigned)(w.bounces - 1));
    } else {
      const uint32_t held = now - w.pressedAtMs;
      Serial.printf("%-10u %-11s release   %5u ms  %u  %s\n", (unsigned)now, w.name,
                    (unsigned)held, (unsigned)(w.bounces - 1), held >= kLongPressMs ? "LONG" : "short");
    }
    w.bounces = 0;
  }
  delay(1);
}
