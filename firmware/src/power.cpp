#include "power.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <soc/soc_caps.h>

#include "board.h"
#include "touch.h"

namespace power {
namespace {

#if SOC_PM_SUPPORT_EXT0_WAKEUP
constexpr bool kHasExt0 = true;
#else
constexpr bool kHasExt0 = false;  // ESP32-C6 (the 7.5" board)
#endif

// Wake sources for light sleep: the touch INT (active low) and the buttons.
void enableInputWakeup() {
  const int touchPin = touch::interruptPin();
  if (touchPin >= 0) gpio_wakeup_enable(gpio_num_t(touchPin), GPIO_INTR_LOW_LEVEL);
  for (int pin : {kBtnNext, kBtnMenu, kBtnPrev}) {
    if (pin >= 0) {
      gpio_wakeup_enable(gpio_num_t(pin),
                         kBtnActiveHigh ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);
    }
  }
  esp_sleep_enable_gpio_wakeup();
}

}  // namespace

void begin() {
  if (kPowerHoldPin >= 0) {
    pinMode(kPowerHoldPin, OUTPUT);
    digitalWrite(kPowerHoldPin, HIGH);
  }
  if (kPowerOffPulsePin >= 0) {
    pinMode(kPowerOffPulsePin, OUTPUT);
    digitalWrite(kPowerOffPulsePin, LOW);
  }
}

Wake wakeReason() {
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT0: return Wake::NextButton;
    case ESP_SLEEP_WAKEUP_EXT1: {  // MENU alone, or any button (active high, or no ext0)
      const uint64_t pins = esp_sleep_get_ext1_wakeup_status();
      if (kBtnNext >= 0 && (pins >> kBtnNext & 1)) return Wake::NextButton;
      if (kBtnPrev >= 0 && (pins >> kBtnPrev & 1)) return Wake::PrevButton;
      return Wake::MenuButton;
    }
    case ESP_SLEEP_WAKEUP_TIMER: return Wake::Timer;
    case ESP_SLEEP_WAKEUP_UNDEFINED: return Wake::PowerOn;  // power-on or reset
    default: return Wake::Other;
  }
}

const char *wakeName(Wake w) {
  switch (w) {
    case Wake::PowerOn: return "power-on/reset";
    case Wake::NextButton: return "NEXT button";
    case Wake::MenuButton: return "MENU button";
    case Wake::PrevButton: return "PREV button";
    case Wake::Timer: return "idle timer";
    case Wake::Other: return "other";
  }
  return "?";
}

uint32_t batteryMv() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(kBatAdc);
  return sum / 16 * kBatDividerNum;
}

bool lightSleep(uint32_t timerSeconds) {
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  enableInputWakeup();
  if (timerSeconds) esp_sleep_enable_timer_wakeup(uint64_t(timerSeconds) * 1000000ULL);
  Serial.flush();
  esp_light_sleep_start();
  return esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER;
}

void deepSleep(uint32_t timerSeconds) {
  if (kLightSleep) {  // M5: off
    Serial.println("power off");
    Serial.flush();
    if (kPowerHoldPin >= 0) digitalWrite(kPowerHoldPin, LOW);
    for (int i = 0; kPowerOffPulsePin >= 0 && i < 5; i++) {  // as M5Unified does it
      digitalWrite(kPowerOffPulsePin, LOW);
      delay(50);
      digitalWrite(kPowerOffPulsePin, HIGH);
      delay(50);
    }
    delay(500);
    // Still here: USB power. Wait for a touch or button, then start over.
    if (kPowerHoldPin >= 0) digitalWrite(kPowerHoldPin, HIGH);
    if (kPowerOffPulsePin >= 0) digitalWrite(kPowerOffPulsePin, LOW);
    lightSleep(0);
    esp_restart();
  }
  // Only what is set below wakes the chip: a nap before this (lightSleep()) left its
  // idle timer and GPIO wake-up on.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (kBtnActiveHigh || !kHasExt0) {
    // Every button on ext1: "any high" (all chips have it), or "any low" on chips
    // without ext0 (the 7.5" board's C6, wired with plain buttons to GND; mode 0).
    // Gravity modules pull their pins down; the internal pull-downs (or -ups) too.
    // A pin that can't wake is left out: one bad pin would refuse the whole mask.
    uint64_t mask = 0;
    for (int pin : {kBtnNext, kBtnMenu, kBtnPrev}) {
      if (pin < 0 || !esp_sleep_is_valid_wakeup_gpio(gpio_num_t(pin))) continue;
      mask |= 1ULL << pin;
      if (kBtnActiveHigh) {
        rtc_gpio_pullup_dis(gpio_num_t(pin));
        rtc_gpio_pulldown_en(gpio_num_t(pin));
      } else {
        rtc_gpio_pullup_en(gpio_num_t(pin));
        rtc_gpio_pulldown_dis(gpio_num_t(pin));
      }
    }
    esp_sleep_enable_ext1_wakeup(mask, kBtnActiveHigh ? ESP_EXT1_WAKEUP_ANY_HIGH
                                                      : esp_sleep_ext1_wakeup_mode_t(0));
  } else {
#if SOC_PM_SUPPORT_EXT0_WAKEUP
    // NEXT: ext0, wake on low (T5-2.13: GPIO39 with an external pull-up).
    if (kBtnNextPullup) {
      rtc_gpio_pullup_en(gpio_num_t(kBtnNext));
      rtc_gpio_pulldown_dis(gpio_num_t(kBtnNext));
    }
    esp_sleep_enable_ext0_wakeup(gpio_num_t(kBtnNext), 0);
#endif
    // MENU (T5-2.13: GPIO0): ext1 with a single pin, so "all low" (ESP32) and
    // "any low" (later chips) both mean "this pin low"; both are mode 0.
    int menuPin = kBtnMenu;  // < 0 on one-button boards
    if (menuPin >= 0) {
      rtc_gpio_pullup_en(gpio_num_t(menuPin));
      rtc_gpio_pulldown_dis(gpio_num_t(menuPin));
      esp_sleep_enable_ext1_wakeup(1ULL << menuPin, esp_sleep_ext1_wakeup_mode_t(0));
    }
  }
  if (timerSeconds) esp_sleep_enable_timer_wakeup(uint64_t(timerSeconds) * 1000000ULL);
  Serial.printf("deep sleep (timer %u s)\n", unsigned(timerSeconds));
  Serial.flush();
  esp_deep_sleep_start();
}

}  // namespace power
