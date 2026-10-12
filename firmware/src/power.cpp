#include "power.h"

#include <Arduino.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#include "board.h"

namespace power {

Wake wakeReason() {
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT0: return Wake::NextButton;
    case ESP_SLEEP_WAKEUP_EXT1: return Wake::MenuButton;
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

void deepSleep(uint32_t timerSeconds) {
  // NEXT (GPIO39): ext0, wake on low; the board has an external pull-up.
  esp_sleep_enable_ext0_wakeup(gpio_num_t(kBtnNext), 0);
  // MENU (GPIO0): ext1 with a single pin, so "all low" = "this pin low".
  rtc_gpio_pullup_en(gpio_num_t(kBtnMenu));
  rtc_gpio_pulldown_dis(gpio_num_t(kBtnMenu));
  esp_sleep_enable_ext1_wakeup(1ULL << kBtnMenu, ESP_EXT1_WAKEUP_ALL_LOW);
  if (timerSeconds) esp_sleep_enable_timer_wakeup(uint64_t(timerSeconds) * 1000000ULL);
  Serial.printf("deep sleep (timer %u s)\n", unsigned(timerSeconds));
  Serial.flush();
  esp_deep_sleep_start();
}

}  // namespace power
