#include "input.h"

#include <Arduino.h>
#include <driver/rtc_io.h>

#include "board.h"
#include "button.h"

namespace input {
namespace {

ButtonTracker next;
ButtonTracker menu;
ClickDecoder single;  // one-button boards
uint32_t activityMs = 0;

bool down(int pin) { return pin >= 0 && digitalRead(pin) == LOW; }

void configure(uint16_t longMs, bool repeat) {
  next.setConfig({25, longMs, repeat ? 250u : 0u});
  menu.setConfig({25, longMs, 0});
  single.setConfig({25, longMs, longMs + 1000u, 300});
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

Key translate(Gesture g) {
  switch (g) {
    case Gesture::Tap: return Key::NextShort;
    case Gesture::DoubleTap: return Key::NextLong;
    case Gesture::Hold: return Key::MenuShort;
    case Gesture::LongHold: return Key::MenuLong;
    case Gesture::None: break;
  }
  return Key::None;
}

}  // namespace

void begin(uint16_t longPressMs) {
  for (int pin : {kBtnNext, kBtnMenu}) {
    if (pin >= 0 && rtc_gpio_is_valid_gpio(gpio_num_t(pin))) rtc_gpio_deinit(gpio_num_t(pin));
  }
  pinMode(kBtnNext, kBtnNextPullup ? INPUT_PULLUP : INPUT);
  if (!kSingleButton) pinMode(kBtnMenu, INPUT_PULLUP);
  configure(longPressMs, false);
  activityMs = millis();
}

void setLongPress(uint16_t ms) {
  configure(ms, next.config().repeatMs != 0);
}

void setRepeat(bool enable) { configure(uint16_t(next.config().longMs), enable); }

Key poll() {
  const uint32_t now = millis();
  Key k;
  bool busy;
  if (kSingleButton) {
    k = translate(single.update(down(kBtnNext), now));
    busy = single.busy();
  } else {
    k = translate(next.update(down(kBtnNext), now), true);
    if (k == Key::None) k = translate(menu.update(down(kBtnMenu), now), false);
    busy = next.isDown() || menu.isDown();
  }
  if (k != Key::None || busy) activityMs = now;
  return k;
}

Key wakePress(int pin, uint32_t timeoutMs) {
  if (pin < 0) return Key::None;
  // Pressed since (about) boot. Already released when we first look (a slow
  // boot, e.g. the S3's PSRAM test, outlasted a tap): count it as short.
  const uint32_t since = down(pin) ? 0 : millis();
  if (kSingleButton) {
    single.startPressed(since);
  } else {
    (pin == kBtnNext ? next : menu).startPressed(since);
  }
  const uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    Key k;
    if (kSingleButton) {
      k = translate(single.update(down(pin), millis()));
    } else {
      ButtonTracker &b = pin == kBtnNext ? next : menu;
      k = translate(b.update(down(pin), millis()), pin == kBtnNext);
    }
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
