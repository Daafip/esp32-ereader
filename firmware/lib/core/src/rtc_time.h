// Clock chip time <-> epoch seconds. The M5 boards switch off when idle, which
// stops the ESP32's own clock; their BM8563 (register-compatible with the PCF8563)
// keeps running and holds UTC. Registers 0x02..0x08 in BCD: seconds (bit 7 = VL,
// the clock stopped since it was last set), minutes, hours, day, weekday,
// month (bit 7 = century), year 00-99.
#pragma once

#include <stdint.h>

constexpr int kRtcRegCount = 7;  // 0x02..0x08

// Days since 1970-01-01 for a proleptic Gregorian date, and back.
int32_t daysFromCivil(int32_t y, unsigned m, unsigned d);
void civilFromDays(int32_t days, int32_t &y, unsigned &m, unsigned &d);

// UTC epoch seconds into the 7 registers (2000-2099, century bit 0).
void encodeRtc(uint32_t epoch, uint8_t regs[kRtcRegCount]);
// The registers as epoch seconds; false if the clock stopped (VL) or a field is
// out of range (a chip that was never set).
bool decodeRtc(const uint8_t regs[kRtcRegCount], uint32_t &epoch);
