#include "input.h"

#include <Arduino.h>
#include <driver/rtc_io.h>

#include "board.h"
#include "button.h"

namespace input {
namespace {

ButtonTracker next;
ButtonTracker menu;
uint32_t activityMs = 0;

bool down(int pin) { return digitalRead(pin) == LOW; }

void configure(uint16_t longMs, bool repeat) {
  next.setConfig({25, longMs, repeat ? 250u : 0u});
  menu.setConfig({25, longMs, 0});
}

Key translate(ButtonEvent e, bool isNext) {
  switch (e) {
    case ButtonEvent::Short: return isNext ? Key::NextShort : Key::MenuShort;
    case ButtonEvent::Long: return isNext ? Key::NextLong : Key::MenuLong;
    case ButtonEvent::Repeat: return isNext ? Key::NextRepeat : Key::None;
    case ButtonEvent::None: break;
  }
  return Key::None;
}

}  // namespace

void begin(uint16_t longPressMs) {
  for (int pin : {kBtnNext, kBtnMenu}) {
    if (rtc_gpio_is_valid_gpio(gpio_num_t(pin))) rtc_gpio_deinit(gpio_num_t(pin));
  }
  pinMode(kBtnNext, INPUT);  // GPIO39: input-only, external pull-up
  pinMode(kBtnMenu, INPUT_PULLUP);
  configure(longPressMs, false);
  activityMs = millis();
}

void setLongPress(uint16_t ms) {
  configure(ms, next.config().repeatMs != 0);
}

void setRepeat(bool enable) { configure(uint16_t(next.config().longMs), enable); }

Key poll() {
  const uint32_t now = millis();
  Key k = translate(next.update(down(kBtnNext), now), true);
  if (k == Key::None) k = translate(menu.update(down(kBtnMenu), now), false);
  if (k != Key::None || next.isDown() || menu.isDown()) activityMs = now;
  return k;
}

Key wakePress(int pin, uint32_t timeoutMs) {
  ButtonTracker &b = pin == kBtnNext ? next : menu;
  b.startPressed(0);  // pressed since (about) boot
  const uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    const Key k = translate(b.update(down(pin), millis()), pin == kBtnNext);
    if (k != Key::None) {
      activityMs = millis();
      return k;
    }
    delay(2);
  }
  return Key::None;
}

bool anyPressed() { return down(kBtnNext) || down(kBtnMenu); }

void waitAllReleased(uint32_t timeoutMs) {
  const uint32_t start = millis();
  while (anyPressed() && millis() - start < timeoutMs) delay(5);
  // let the trackers see the release so it doesn't become an event later
  for (int i = 0; i < 10; i++) {
    poll();
    delay(5);
  }
}

uint32_t lastActivityMs() { return activityMs; }

}  // namespace input
