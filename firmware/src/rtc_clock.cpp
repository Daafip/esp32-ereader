#include "rtc_clock.h"

#include "board.h"

#ifdef BOARD_HAS_RTC

#include <Arduino.h>
#include <Wire.h>
#include <sys/time.h>
#include <time.h>

#include "reading_stats.h"
#include "rtc_time.h"

namespace rtcclock {
namespace {

constexpr uint8_t kRegControl1 = 0x00;  // bit 5 STOP
constexpr uint8_t kRegSeconds = 0x02;   // first of kRtcRegCount time registers

bool begin() { return Wire.begin(kTouchSda, kTouchScl, 400000); }  // shared with touch

}  // namespace

void restore() {
  if (uint32_t(time(nullptr)) >= kClockValid || !begin()) return;
  uint8_t r[kRtcRegCount];
  Wire.beginTransmission(kRtcAddress);
  Wire.write(kRegSeconds);
  if (Wire.endTransmission(false) != 0) return;
  if (Wire.requestFrom(kRtcAddress, uint8_t(kRtcRegCount)) != kRtcRegCount) return;
  for (uint8_t &b : r) b = uint8_t(Wire.read());
  uint32_t epoch = 0;
  if (!decodeRtc(r, epoch) || epoch < kClockValid) return;
  timeval tv = {time_t(epoch), 0};
  settimeofday(&tv, nullptr);
  Serial.printf("clock: %u from the RTC\n", unsigned(epoch));
}

void save(uint32_t epoch) {
  if (!begin()) return;
  uint8_t r[kRtcRegCount];
  encodeRtc(epoch, r);
  Wire.beginTransmission(kRtcAddress);
  Wire.write(kRegControl1);
  Wire.write(uint8_t(0));  // running, no test mode
  if (Wire.endTransmission() != 0) return;
  Wire.beginTransmission(kRtcAddress);
  Wire.write(kRegSeconds);
  Wire.write(r, kRtcRegCount);  // VL cleared with the seconds
  Wire.endTransmission();
}

}  // namespace rtcclock

#else  // no clock chip

namespace rtcclock {
void restore() {}
void save(uint32_t) {}
}  // namespace rtcclock

#endif
