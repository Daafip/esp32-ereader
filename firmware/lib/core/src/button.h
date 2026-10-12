// Debounced button with short / long press and optional auto-repeat.
// Pure logic: feed it the raw level and the time; the caller owns the GPIO.
#pragma once

#include <stdint.h>

enum class ButtonEvent : uint8_t {
  None,
  Short,   // released before the long-press threshold
  Long,    // held for longMs (fires once, while still held)
  Repeat,  // still held, every repeatMs after Long (if repeatMs > 0)
};

class ButtonTracker {
 public:
  struct Config {
    uint32_t debounceMs = 25;
    uint32_t longMs = 600;
    uint32_t repeatMs = 0;  // 0 = no auto-repeat
  };

  ButtonTracker() = default;
  explicit ButtonTracker(const Config &config) : config_(config) {}

  void setConfig(const Config &config) { config_ = config; }
  const Config &config() const { return config_; }

  // `pressed` is the raw (undebounced) state. Returns at most one event per call.
  ButtonEvent update(bool pressed, uint32_t nowMs);

  // Start in the pressed state, as if the press began at `sinceMs` (the button
  // that woke the chip from deep sleep).
  void startPressed(uint32_t sinceMs);

  bool isDown() const { return stable_; }

 private:
  Config config_;
  bool stable_ = false;
  bool raw_ = false;
  bool longFired_ = false;
  uint32_t rawChangeMs_ = 0;
  uint32_t pressStartMs_ = 0;
  uint32_t nextRepeatMs_ = 0;
};
