// Battery, wake-up cause and ESP32 deep sleep.
#pragma once

#include <stdint.h>

namespace power {

enum class Wake : uint8_t { PowerOn, NextButton, MenuButton, Timer, Other };

Wake wakeReason();
const char *wakeName(Wake w);

// Battery voltage in mV (16 samples, divider corrected). Below ~2500 mV the
// reading is meaningless (no battery, USB only).
uint32_t batteryMv();

// Deep sleep until NEXT (ext0) or MENU (ext1) is pressed, or after
// `timerSeconds` (0 = no timer). Does not return.
[[noreturn]] void deepSleep(uint32_t timerSeconds);

}  // namespace power
