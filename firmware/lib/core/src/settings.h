// User settings and their allowed values (Settings menu cycles through them).
#pragma once

#include <stddef.h>
#include <stdint.h>

// How a page turn after deep sleep gets its "old" image for a partial refresh
// (Phase 4 spike, see PLAN.md):
enum class WakeRefresh : uint8_t {
  Rewrite,  // send the previous image (kept in RTC RAM) as "old", panel power off in sleep
  Retain,   // keep the panel powered in its RAM-retaining sleep, send only the new image
  Full,     // fast/full refresh on every wake
};

struct Settings {
  uint8_t fullEvery = 10;      // full refresh after this many partial ones; 0 = never
  uint16_t longPressMs = 600;  // short/long press threshold
  uint8_t idleMinutes = 10;    // awake/idle time before the sleep screen
  bool deepSleep = true;       // deep sleep after every page turn
  WakeRefresh wakeRefresh = WakeRefresh::Rewrite;
};

enum class SettingId : uint8_t { FullEvery, LongPress, IdleMinutes, DeepSleep, WakeRefresh, Count };

constexpr int kSettingCount = int(SettingId::Count);

// Advances one setting to its next allowed value (wrapping).
void cycleSetting(Settings &s, SettingId id);

// Row text for the Settings list: label left, current value right.
const char *settingLabel(SettingId id);                                   // "Full refresh"
void settingValue(const Settings &s, SettingId id, char *out, size_t outSize);  // "every 10"

// Replaces values that aren't in the allowed lists (e.g. from old NVS data) by defaults.
void sanitizeSettings(Settings &s);

const char *wakeRefreshName(WakeRefresh w);
