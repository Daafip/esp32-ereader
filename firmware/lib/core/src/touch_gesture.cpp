#include "touch_gesture.h"

#include <stdlib.h>

TouchEvent TouchTracker::update(bool down, int x, int y, uint32_t now) {
  TouchEvent e;
  if (down && !down_) {
    down_ = true;
    moved_ = longFired_ = false;
    x0_ = x_ = int16_t(x);
    y0_ = y_ = int16_t(y);
    t0_ = now;
    return e;
  }
  if (down) {
    x_ = int16_t(x);
    y_ = int16_t(y);
    if (abs(x_ - x0_) > config_.slop || abs(y_ - y0_) > config_.slop) moved_ = true;
    if (!moved_ && !longFired_ && now - t0_ >= config_.longMs) {
      longFired_ = true;
      e = {TouchKind::LongPress, x0_, y0_};
    }
    return e;
  }
  if (!down_) return e;
  down_ = false;  // released: x/y of a release sample are not used (often 0)
  if (longFired_) return e;
  const int dx = x_ - x0_, dy = y_ - y0_;
  e.x = x0_;
  e.y = y0_;
  if (abs(dx) >= config_.swipeMin && abs(dx) > 2 * abs(dy)) {
    e.kind = dx < 0 ? TouchKind::SwipeLeft : TouchKind::SwipeRight;
  } else if (abs(dy) >= config_.swipeMin && abs(dy) > 2 * abs(dx)) {
    e.kind = dy < 0 ? TouchKind::SwipeUp : TouchKind::SwipeDown;
  } else if (!moved_) {
    e.kind = TouchKind::Tap;
  }
  return e;
}

ReadingTap readingTap(int x, int y, int w, int h) {
  if (y < h / 8) return ReadingTap::Menu;
  return x < w / 3 ? ReadingTap::Prev : ReadingTap::Next;
}
