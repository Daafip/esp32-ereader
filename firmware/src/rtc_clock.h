// Battery-backed clock chip (BM8563 on the M5 boards). The ESP32's own clock runs
// through deep and light sleep but stops when the M5 boards switch off; the chip
// keeps UTC for the reading statistics. No-ops on boards without one.
#pragma once

#include <stdint.h>

namespace rtcclock {

void restore();             // system clock from the chip, if ours isn't set and it holds a time
void save(uint32_t epoch);  // UTC, after the phone set the time

}  // namespace rtcclock
