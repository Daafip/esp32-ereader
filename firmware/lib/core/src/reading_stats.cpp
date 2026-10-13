#include "reading_stats.h"

int32_t localDay(uint32_t now, int32_t tz) {
  if (now < kClockValid) return -1;
  return int32_t((int64_t(now) + tz) / 86400);
}

void recordPage(ReadingStats &st, uint32_t now, int32_t tz) {
  // Time on the previous page, unless it was a pause (or the clock jumped).
  uint32_t secs = 0;
  if (st.lastTurn && now > st.lastTurn && now - st.lastTurn <= kMaxPageSeconds) {
    secs = now - st.lastTurn;
  }
  st.lastTurn = now;
  st.pages++;
  st.seconds += secs;
  const int32_t day = localDay(now, tz);
  if (day < 0) return;
  if (day != st.lastDay) {
    st.streak = st.lastDay >= 0 && day == st.lastDay + 1 ? st.streak + 1 : 1;
    if (st.streak > st.bestStreak) st.bestStreak = st.streak;
    st.daysRead++;
    st.lastDay = day;
    st.dayPages = st.daySeconds = 0;
  }
  st.dayPages++;
  st.daySeconds += secs;
}

uint32_t todayPages(const ReadingStats &st, uint32_t now, int32_t tz) {
  const int32_t day = localDay(now, tz);
  return day >= 0 && day == st.lastDay ? st.dayPages : 0;
}

uint32_t todaySeconds(const ReadingStats &st, uint32_t now, int32_t tz) {
  const int32_t day = localDay(now, tz);
  return day >= 0 && day == st.lastDay ? st.daySeconds : 0;
}

uint32_t currentStreak(const ReadingStats &st, uint32_t now, int32_t tz) {
  const int32_t day = localDay(now, tz);
  if (day < 0) return st.streak;  // unknown today: show the last known streak
  return st.lastDay >= 0 && day - st.lastDay <= 1 ? st.streak : 0;
}
