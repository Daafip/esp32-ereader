#include "policy.h"

Refresh decideRefresh(bool wantPartial, bool chapterChanged, bool shownValid,
                      uint16_t partialsSinceFull, uint8_t fullEvery) {
  if (!wantPartial || !shownValid || chapterChanged) return Refresh::Full;
  if (fullEvery && partialsSinceFull >= fullEvery) return Refresh::Full;
  return Refresh::Partial;
}

int batteryPercent(uint32_t mv) {
  struct Point {
    uint32_t mv;
    int pct;
  };
  // Typical single-cell LiPo discharge curve at low current.
  static const Point kCurve[] = {
      {kBatteryEmptyMv, 0}, {3600, 5},  {3700, 15}, {3750, 25}, {3800, 40},
      {3850, 55},           {3900, 65}, {4000, 80}, {4100, 92}, {4200, 100},
  };
  constexpr int n = sizeof(kCurve) / sizeof(kCurve[0]);
  if (mv <= kCurve[0].mv) return 0;
  if (mv >= kCurve[n - 1].mv) return 100;
  for (int i = 1; i < n; i++) {
    if (mv <= kCurve[i].mv) {
      const Point &a = kCurve[i - 1];
      const Point &b = kCurve[i];
      return a.pct + int((mv - a.mv) * uint32_t(b.pct - a.pct) / (b.mv - a.mv));
    }
  }
  return 100;
}
