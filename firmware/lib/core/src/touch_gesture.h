// Touch screen gestures from raw finger positions: tap, long press, swipes.
// Pure logic: feed it "finger down at (x, y)" samples and the time.
#pragma once

#include <stdint.h>

enum class TouchKind : uint8_t {
  None,
  Tap,
  LongPress,  // held still for longMs (fires once, while still down)
  SwipeLeft,  // finger moved left (next page)
  SwipeRight,
  SwipeUp,
  SwipeDown,
};

struct TouchEvent {
  TouchKind kind = TouchKind::None;
  int16_t x = 0;  // where the finger went down
  int16_t y = 0;
};

class TouchTracker {
 public:
  struct Config {
    uint32_t longMs = 600;
    int16_t slop = 24;      // movement still counted as "still" (px)
    int16_t swipeMin = 80;  // shortest swipe (px), mostly along one axis
  };

  TouchTracker() = default;
  explicit TouchTracker(const Config &config) : config_(config) {}
  void setConfig(const Config &config) { config_ = config; }

  // At most one event per call.
  TouchEvent update(bool down, int x, int y, uint32_t nowMs);
  bool isDown() const { return down_; }

 private:
  Config config_;
  bool down_ = false;
  bool moved_ = false;
  bool longFired_ = false;
  int16_t x0_ = 0, y0_ = 0, x_ = 0, y_ = 0;
  uint32_t t0_ = 0;
};

// Reading screen: what a tap at (x, y) on a w x h page means. Left third =
// previous page, top eighth = menu, everything else = next page.
enum class ReadingTap : uint8_t { Next, Prev, Menu };
ReadingTap readingTap(int x, int y, int w, int h);
