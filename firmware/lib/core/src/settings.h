// User settings: each one is a short list of allowed values (options). The
// Settings menu cycles through them, NVS stores the option index, and the web
// page shows the same lists (see transfer.cpp /api/settings).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "i18n.h"

// How a page turn after deep sleep gets its "old" image for a partial refresh
// (Phase 4 spike, see PLAN.md):
enum class WakeRefresh : uint8_t {
  Rewrite,  // send the previous image (kept in RTC RAM) as "old", panel power off in sleep
  Retain,   // keep the panel powered in its RAM-retaining sleep, send only the new image
  Full,     // fast/full refresh on every wake
};

// What the reader shows while asleep (new ones at the end: NVS stores the index).
enum class SleepScreen : uint8_t { Book, Cover, Pictures, Page, PictureBook };
enum class StatusBarMode : uint8_t { Full, Minimal, Hidden };
// The second gesture of MENU while reading (two buttons: hold MENU; one button:
// long hold; touch: long press).
enum class HoldAction : uint8_t { Sleep, Bookmark, Lock, Menu };

struct Settings {
  SleepScreen sleepScreen = SleepScreen::Book;
  StatusBarMode statusBar = StatusBarMode::Full;
  bool flip = false;  // screen upside down (the reader held the other way round)
  HoldAction holdAction = HoldAction::Sleep;
  bool lockOnSleep = false;
  Lang language = Lang::En;
  uint8_t fullEvery = 10;      // full refresh after this many partial ones; 0 = never
  uint16_t longPressMs = 600;  // short/long press threshold
  uint8_t idleMinutes = 10;    // awake/idle time before the sleep screen
  bool deepSleep = true;       // sleep after every page turn
  WakeRefresh wakeRefresh = WakeRefresh::Rewrite;
};

enum class SettingId : uint8_t {
  SleepScreen,
  StatusBar,
  Flip,
  HoldAction,
  LockOnSleep,
  Language,
  FullEvery,
  LongPress,
  IdleMinutes,
  DeepSleep,
  WakeRefresh,
  Count
};

constexpr int kSettingCount = int(SettingId::Count);

int settingOptionCount(SettingId id);
int settingIndex(const Settings &s, SettingId id);  // current option
void setSettingIndex(Settings &s, SettingId id, int index);  // ignored if out of range
void cycleSetting(Settings &s, SettingId id);  // next option, wrapping

// Stable ASCII name ("sleepScreen", "fullEvery", ...): NVS keys and the web API.
const char *settingKey(SettingId id);
bool settingFromKey(const char *key, SettingId &out);

// Row text for the Settings list, in the current language: label left, value
// right. `holdLabel` names the hold gesture of this board (S::SetHoldMenu,
// S::SetLongHold or S::SetLongPress).
const char *settingLabel(SettingId id, S holdLabel = S::SetHoldMenu);
void settingOptionText(SettingId id, int index, char *out, size_t outSize);
void settingValue(const Settings &s, SettingId id, char *out, size_t outSize);

const char *wakeRefreshName(WakeRefresh w);  // English, for the serial log
