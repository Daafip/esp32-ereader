// Reading statistics: pages turned, reading time, days read and the daily streak.
// Days need the reader's clock, which the phone sets over Wi-Fi; without it only
// pages and time count.
#pragma once

#include <stdint.h>

constexpr uint32_t kClockValid = 1700000000;  // epoch seconds; earlier = clock not set
constexpr uint32_t kMaxPageSeconds = 180;     // a longer pause on one page isn't reading

struct ReadingStats {
  uint32_t pages = 0;    // pages turned forward, lifetime
  uint32_t seconds = 0;  // reading time, lifetime
  uint32_t daysRead = 0;
  uint32_t streak = 0;   // consecutive days up to lastDay
  uint32_t bestStreak = 0;
  int32_t lastDay = -1;  // local day number of the last reading day, -1 = none
  uint32_t dayPages = 0;    // on lastDay
  uint32_t daySeconds = 0;  // on lastDay
  uint32_t lastTurn = 0;    // clock seconds of the previous page turn, 0 = none
};

// Local day number for a clock time; -1 if the clock isn't set.
int32_t localDay(uint32_t now, int32_t tzOffsetSeconds);

// A page turned forward at `now` (clock seconds).
void recordPage(ReadingStats &st, uint32_t now, int32_t tzOffsetSeconds);

// Figures as of `now`: today's pages/seconds are 0 and the streak is broken when
// the last reading day is long gone.
uint32_t todayPages(const ReadingStats &st, uint32_t now, int32_t tz);
uint32_t todaySeconds(const ReadingStats &st, uint32_t now, int32_t tz);
uint32_t currentStreak(const ReadingStats &st, uint32_t now, int32_t tz);
