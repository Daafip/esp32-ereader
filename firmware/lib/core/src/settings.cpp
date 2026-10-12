#include "settings.h"

#include <stdio.h>

namespace {

const uint8_t kFullEvery[] = {5, 10, 15, 20, 30, 0};
const uint16_t kLongPress[] = {400, 500, 600, 800, 1000};
const uint8_t kIdle[] = {2, 5, 10, 20, 30, 60};

template <typename T, size_t N>
T nextValue(const T (&values)[N], T current) {
  for (size_t i = 0; i < N; i++) {
    if (values[i] == current) return values[(i + 1) % N];
  }
  return values[0];
}

template <typename T, size_t N>
bool allowed(const T (&values)[N], T v) {
  for (size_t i = 0; i < N; i++) {
    if (values[i] == v) return true;
  }
  return false;
}

}  // namespace

const char *wakeRefreshName(WakeRefresh w) {
  switch (w) {
    case WakeRefresh::Rewrite: return "rewrite";
    case WakeRefresh::Retain: return "retain";
    case WakeRefresh::Full: return "full";
  }
  return "?";
}

void cycleSetting(Settings &s, SettingId id) {
  switch (id) {
    case SettingId::FullEvery: s.fullEvery = nextValue(kFullEvery, s.fullEvery); break;
    case SettingId::LongPress: s.longPressMs = nextValue(kLongPress, s.longPressMs); break;
    case SettingId::IdleMinutes: s.idleMinutes = nextValue(kIdle, s.idleMinutes); break;
    case SettingId::DeepSleep: s.deepSleep = !s.deepSleep; break;
    case SettingId::WakeRefresh:
      s.wakeRefresh = WakeRefresh((uint8_t(s.wakeRefresh) + 1) % 3);
      break;
    case SettingId::Count: break;
  }
}

void describeSetting(const Settings &s, SettingId id, char *out, size_t n) {
  switch (id) {
    case SettingId::FullEvery:
      if (s.fullEvery) {
        snprintf(out, n, "Full refresh: every %u", unsigned(s.fullEvery));
      } else {
        snprintf(out, n, "Full refresh: chapters only");
      }
      break;
    case SettingId::LongPress: snprintf(out, n, "Long press: %u ms", unsigned(s.longPressMs)); break;
    case SettingId::IdleMinutes: snprintf(out, n, "Idle sleep: %u min", unsigned(s.idleMinutes)); break;
    case SettingId::DeepSleep: snprintf(out, n, "Sleep after page: %s", s.deepSleep ? "on" : "off"); break;
    case SettingId::WakeRefresh:
      snprintf(out, n, "Wake refresh: %s", wakeRefreshName(s.wakeRefresh));
      break;
    case SettingId::Count: snprintf(out, n, "?"); break;
  }
}

void sanitizeSettings(Settings &s) {
  const Settings d;
  if (!allowed(kFullEvery, s.fullEvery)) s.fullEvery = d.fullEvery;
  if (!allowed(kLongPress, s.longPressMs)) s.longPressMs = d.longPressMs;
  if (!allowed(kIdle, s.idleMinutes)) s.idleMinutes = d.idleMinutes;
  if (uint8_t(s.wakeRefresh) > uint8_t(WakeRefresh::Full)) s.wakeRefresh = d.wakeRefresh;
}
