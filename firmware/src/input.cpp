#include "input.h"

#include <Arduino.h>
#include <driver/rtc_io.h>

#include "board.h"
#include "button.h"
#include "touch.h"

namespace input {
namespace {

ButtonTracker next;
ButtonTracker menu;
ButtonTracker prev;   // M5Paper
ClickDecoder single;  // one-button boards
TouchTracker finger;  // touch boards
TouchEvent touchEvent;
bool hasTouch = false;
uint32_t activityMs = 0;

bool down(int pin) { return pin >= 0 && digitalRead(pin) == (kBtnActiveHigh ? HIGH : LOW); }

// Settings -> Flip screen on a board with up/down keys (M5Paper's switch): held
// the other way round, "up" and "down" swap.
bool flipped = false;
int nextPin() { return flipped && kBtnPrev >= 0 ? kBtnPrev : kBtnNext; }
int prevPin() { return flipped && kBtnPrev >= 0 ? kBtnNext : kBtnPrev; }

void configure(uint16_t longMs, bool repeat) {
  next.setConfig({25, longMs, repeat ? 250u : 0u});
  prev.setConfig({25, longMs, repeat ? 250u : 0u});
  menu.setConfig({25, longMs, 0});
  single.setConfig({25, longMs, longMs + 1000u, 300});
  finger.setConfig({longMs, int16_t(12 * kUiScale), int16_t(40 * kUiScale)});
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

// PREV (M5Paper's switch, the 7.5" board's button 1): back, short or long.
Key translatePrev(ButtonEvent e) {
  switch (e) {
    case ButtonEvent::Short:
    case ButtonEvent::Long: return Key::NextLong;
    case ButtonEvent::Repeat: return Key::NextRepeat;
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
  for (int pin : {kBtnNext, kBtnMenu, kBtnPrev}) {
    if (pin >= 0 && rtc_gpio_is_valid_gpio(gpio_num_t(pin))) rtc_gpio_deinit(gpio_num_t(pin));
  }
  if (kBtnActiveHigh) {  // pulled down when released (the 7.5" board's modules do it too)
    for (int pin : {kBtnNext, kBtnMenu, kBtnPrev}) {
      if (pin >= 0) pinMode(pin, INPUT_PULLDOWN);
    }
  } else {
    if (kBtnNext >= 0) pinMode(kBtnNext, kBtnNextPullup ? INPUT_PULLUP : INPUT);
    // GPIO34-39 are input-only without pull-ups; those boards have external ones
    if (kBtnMenu >= 0) pinMode(kBtnMenu, kBtnMenu >= 34 ? INPUT : INPUT_PULLUP);
    if (kBtnPrev >= 0) pinMode(kBtnPrev, kBtnPrev >= 34 ? INPUT : INPUT_PULLUP);
  }
  hasTouch = touch::begin();
  configure(longPressMs, false);
  activityMs = millis();
}

void setLongPress(uint16_t ms) {
  configure(ms, next.config().repeatMs != 0);
}

void setFlipped(bool on) { flipped = on; }

void setRepeat(bool enable) { configure(uint16_t(next.config().longMs), enable); }

Key poll() {
  const uint32_t now = millis();
  Key k = Key::None;
  bool busy = false;
  if (kSingleButton) {
    k = translate(single.update(down(kBtnNext), now));
    busy = single.busy();
  } else {
    k = translate(next.update(down(nextPin()), now), true);
    if (k == Key::None) k = translate(menu.update(down(kBtnMenu), now), false);
    if (k == Key::None) k = translatePrev(prev.update(down(prevPin()), now));
    busy = next.isDown() || menu.isDown() || prev.isDown();
  }
  if (hasTouch) {
    int x = 0, y = 0;
    const bool fingerDown = touch::read(x, y);
    const TouchEvent e = finger.update(fingerDown, x, y, now);
    if (k == Key::None && e.kind != TouchKind::None) {
      touchEvent = e;
      k = Key::Touch;
    }
    busy = busy || finger.isDown();
  }
  if (k != Key::None || busy) activityMs = now;
  return k;
}

TouchEvent lastTouch() { return touchEvent; }

Key wakePress(int pin, uint32_t timeoutMs) {
  if (pin < 0) return Key::None;
  // Pressed since (about) boot. Already released when we first look (a slow
  // boot, e.g. the S3's PSRAM test, outlasted a tap): count it as short.
  const uint32_t since = down(pin) ? 0 : millis();
  // Which role the pin has now: flipped, NEXT and PREV swap (setFlipped()).
  const bool isNext = pin == nextPin(), isPrev = !isNext && pin == prevPin();
  ButtonTracker &b = isNext ? next : isPrev ? prev : menu;
  if (kSingleButton) {
    single.startPressed(since);
  } else {
    b.startPressed(since);
  }
  const uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    Key k;
    if (kSingleButton) {
      k = translate(single.update(down(pin), millis()));
    } else if (isPrev) {
      k = translatePrev(b.update(down(pin), millis()));
    } else {
      k = translate(b.update(down(pin), millis()), isNext);
    }
    if (k != Key::None) {
      activityMs = millis();
      return k;
    }
    delay(2);
  }
  return Key::None;
}

bool anyPressed() {
  int x, y;
  return down(kBtnNext) || down(kBtnMenu) || down(kBtnPrev) || (hasTouch && touch::read(x, y));
}

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
void markActivity() { activityMs = millis(); }

}  // namespace input
