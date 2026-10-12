#include "button.h"

ButtonEvent ButtonTracker::update(bool pressed, uint32_t now) {
  if (pressed != raw_) {
    raw_ = pressed;
    rawChangeMs_ = now;
  }
  if (raw_ != stable_ && now - rawChangeMs_ >= config_.debounceMs) {
    stable_ = raw_;
    if (stable_) {
      pressStartMs_ = rawChangeMs_;
      longFired_ = false;
    } else {
      const bool wasLong = longFired_;
      longFired_ = false;
      return wasLong ? ButtonEvent::None : ButtonEvent::Short;
    }
  }
  if (!stable_) return ButtonEvent::None;
  if (!longFired_ && now - pressStartMs_ >= config_.longMs) {
    longFired_ = true;
    nextRepeatMs_ = now + config_.repeatMs;
    return ButtonEvent::Long;
  }
  if (longFired_ && config_.repeatMs && int32_t(now - nextRepeatMs_) >= 0) {
    nextRepeatMs_ = now + config_.repeatMs;
    return ButtonEvent::Repeat;
  }
  return ButtonEvent::None;
}

void ButtonTracker::startPressed(uint32_t sinceMs) {
  raw_ = stable_ = true;
  rawChangeMs_ = pressStartMs_ = sinceMs;
  longFired_ = false;
}
