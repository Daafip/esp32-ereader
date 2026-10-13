#include "rtc_time.h"

namespace {

uint8_t toBcd(unsigned v) { return uint8_t(((v / 10) << 4) | (v % 10)); }

// false for a nibble above 9
bool fromBcd(uint8_t b, unsigned &v) {
  if ((b & 0x0F) > 9 || (b >> 4) > 9) return false;
  v = (b >> 4) * 10 + (b & 0x0F);
  return true;
}

}  // namespace

// Howard Hinnant's days_from_civil / civil_from_days.
int32_t daysFromCivil(int32_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = unsigned(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + int32_t(doe) - 719468;
}

void civilFromDays(int32_t days, int32_t &y, unsigned &m, unsigned &d) {
  days += 719468;
  const int32_t era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned doe = unsigned(days - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = int32_t(yoe) + era * 400 + (m <= 2);
}

void encodeRtc(uint32_t epoch, uint8_t regs[kRtcRegCount]) {
  const int32_t days = int32_t(epoch / 86400);
  const uint32_t secs = epoch % 86400;
  int32_t y;
  unsigned m, d;
  civilFromDays(days, y, m, d);
  regs[0] = toBcd(secs % 60);
  regs[1] = toBcd(secs / 60 % 60);
  regs[2] = toBcd(secs / 3600);
  regs[3] = toBcd(d);
  regs[4] = uint8_t((days + 4) % 7);  // 1970-01-01 was a Thursday (4)
  regs[5] = toBcd(m);
  regs[6] = toBcd(unsigned(y % 100));
}

bool decodeRtc(const uint8_t regs[kRtcRegCount], uint32_t &epoch) {
  if (regs[0] & 0x80) return false;  // VL: the clock stopped
  unsigned s, mi, h, d, mo, y;
  if (!fromBcd(regs[0] & 0x7F, s) || !fromBcd(regs[1] & 0x7F, mi) ||
      !fromBcd(regs[2] & 0x3F, h) || !fromBcd(regs[3] & 0x3F, d) ||
      !fromBcd(regs[5] & 0x1F, mo) || !fromBcd(regs[6], y)) {
    return false;
  }
  if (s > 59 || mi > 59 || h > 23 || d < 1 || d > 31 || mo < 1 || mo > 12) return false;
  const int32_t days = daysFromCivil(int32_t(2000 + y), mo, d);
  epoch = uint32_t(days) * 86400 + h * 3600 + mi * 60 + s;
  return true;
}
