// Reading statistics: pages turned, reading time, days read and the daily streak.
// Days need the reader's clock, which the phone sets over Wi-Fi; without it only
// pages and time count.
#pragma once

#include <stdint.h>

#include <type_traits>

constexpr uint32_t kClockValid = 1700000000;  // epoch seconds; earlier = clock not set
constexpr uint32_t kMaxPageSeconds = 180;     // a longer pause on one page isn't reading

// All zero = nothing read yet: `ReadingStats st{};`. No default member values, so a
// struct holding it stays trivial: the firmware keeps one in RTC memory
// (RTC_DATA_ATTR), where a constructor would run on every wake from deep sleep and
// wipe it.
struct ReadingStats {
  uint32_t pages;    // pages turned forward, lifetime
  uint32_t seconds;  // reading time, lifetime
  uint32_t daysRead;
  uint32_t streak;   // consecutive days up to lastDay
  uint32_t bestStreak;
  int32_t lastDay;      // local day number of the last reading day; <= 0 = none (day 0
                        // is 1970-01-01, before any set clock; -1 from older firmware)
  uint32_t dayPages;    // on lastDay
  uint32_t daySeconds;  // on lastDay
  uint32_t lastTurn;    // clock seconds of the previous page turn, 0 = none
};
static_assert(std::is_trivially_default_constructible<ReadingStats>::value,
              "ReadingStats lives in RTC memory: no constructor");

// Local day number for a clock time; -1 if the clock isn't set.
int32_t localDay(uint32_t now, int32_t tzOffsetSeconds);

// A page turned forward at `now` (clock seconds).
void recordPage(ReadingStats &st, uint32_t now, int32_t tzOffsetSeconds);

// Figures as of `now`: today's pages/seconds are 0 and the streak is broken when
// the last reading day is long gone.
uint32_t todayPages(const ReadingStats &st, uint32_t now, int32_t tz);
uint32_t todaySeconds(const ReadingStats &st, uint32_t now, int32_t tz);
uint32_t currentStreak(const ReadingStats &st, uint32_t now, int32_t tz);
