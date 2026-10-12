// Small decision functions: refresh mode, battery level.
#pragma once

#include <stdint.h>

enum class Refresh : uint8_t { Full, Fast, Partial };

// Partial refresh for page turns; a full refresh to clear ghosting every
// `fullEvery` partials (0 = never forced), on a chapter change, or when the
// "old" image needed for a partial update is unknown.
Refresh decideRefresh(bool wantPartial, bool chapterChanged, bool shownValid,
                      uint16_t partialsSinceFull, uint8_t fullEvery);

constexpr uint32_t kBatteryLowMv = 3400;    // show the low-battery screen and stop
constexpr uint32_t kBatteryEmptyMv = 3300;  // 0 %

// LiPo voltage under light load -> 0..100 %.
int batteryPercent(uint32_t mv);
