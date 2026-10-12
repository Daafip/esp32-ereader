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

Gesture ClickDecoder::update(bool pressed, uint32_t now) {
  if (pressed != raw_) {
    raw_ = pressed;
    rawChangeMs_ = now;
  }
  if (raw_ != stable_ && now - rawChangeMs_ >= config_.debounceMs) {
    stable_ = raw_;
    if (stable_) {
      pressStartMs_ = rawChangeMs_;
      longFired_ = false;
      second_ = tapPending_;
      tapPending_ = false;
    } else {
      const uint32_t held = rawChangeMs_ - pressStartMs_;
      const bool wasSecond = second_;
      second_ = false;
      if (longFired_) return Gesture::None;
      if (held >= config_.holdMs) return Gesture::Hold;
      if (wasSecond) return Gesture::DoubleTap;
      tapPending_ = true;
      releaseMs_ = rawChangeMs_;
      return Gesture::None;
    }
  }
  if (stable_) {
    const uint32_t held = now - pressStartMs_;
    if (second_ && held >= config_.holdMs) {
      second_ = false;  // not a double tap: report the first tap, this press goes on
      return Gesture::Tap;
    }
    if (!longFired_ && held >= config_.longHoldMs) {
      longFired_ = true;
      return Gesture::LongHold;
    }
  } else if (tapPending_ && !raw_ && now - releaseMs_ >= config_.doubleTapMs) {
    tapPending_ = false;
    return Gesture::Tap;
  }
  return Gesture::None;
}

void ClickDecoder::startPressed(uint32_t sinceMs) {
  raw_ = stable_ = true;
  rawChangeMs_ = pressStartMs_ = sinceMs;
  longFired_ = tapPending_ = second_ = false;
}
