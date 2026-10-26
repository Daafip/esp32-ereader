// Battery, wake-up cause, sleep and power-off.
//
// T5 boards and the 7.5": deep sleep between pages and when idle; a button wakes
// the chip (ext0/ext1) and setup() handles the press.
// M5 boards (kLightSleep): light sleep between pages (RAM and the panel's image
// stay, a touch or button wakes, loop() goes on); "sleep" switches the device
// off (power hold pin low / power-off pulses), the power button starts it again.
#pragma once

#include <stdint.h>

namespace power {

enum class Wake : uint8_t { PowerOn, NextButton, MenuButton, PrevButton, Timer, Other };

// First thing at boot: latch the power on (M5Paper GPIO2), keep PaperS3's
// power-off pin low.
void begin();

Wake wakeReason();
const char *wakeName(Wake w);

// Battery voltage in mV (16 samples, divider corrected). Below ~2500 mV the
// reading is meaningless (no battery, USB only).
uint32_t batteryMv();

// Light sleep until a touch or button, or after `timerSeconds` (0 = no timer).
// Returns true when woken by input, false by the timer. kLightSleep boards only.
bool lightSleep(uint32_t timerSeconds);

// T5 boards: deep sleep until NEXT (ext0) or MENU (ext1, two-button boards) is
// pressed, or after `timerSeconds` (0 = no timer). Active-high buttons (7.5"): any
// of them (ext1, any high; the C6 has no ext0).
// M5 boards: switch the device off (the timer is ignored). Still powered over
// USB: light sleep until a touch or button, then restart.
// Does not return.
[[noreturn]] void deepSleep(uint32_t timerSeconds);

}  // namespace power
