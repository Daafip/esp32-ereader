#include "settings.h"

#include <stdio.h>
#include <string.h>

namespace {

const uint8_t kFullEvery[] = {5, 10, 15, 20, 30, 0};
const uint16_t kLongPress[] = {400, 500, 600, 800, 1000};
const uint8_t kIdle[] = {2, 5, 10, 20, 30, 60};

template <typename T, size_t N>
int indexOf(const T (&values)[N], T v) {
  for (size_t i = 0; i < N; i++) {
    if (values[i] == v) return int(i);
  }
  return -1;
}

template <typename T, size_t N>
constexpr int count(const T (&)[N]) {
  return int(N);
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

int settingOptionCount(SettingId id) {
  switch (id) {
    case SettingId::SleepScreen: return 4;
    case SettingId::StatusBar: return 3;
    case SettingId::Flip: return 2;
    case SettingId::HoldAction: return 4;
    case SettingId::LockOnSleep: return 2;
    case SettingId::Language: return int(Lang::Count);
    case SettingId::FullEvery: return count(kFullEvery);
    case SettingId::LongPress: return count(kLongPress);
    case SettingId::IdleMinutes: return count(kIdle);
    case SettingId::DeepSleep: return 2;
    case SettingId::WakeRefresh: return 3;
    case SettingId::Count: break;
  }
  return 0;
}

int settingIndex(const Settings &s, SettingId id) {
  int i = 0;
  switch (id) {
    case SettingId::SleepScreen: i = int(s.sleepScreen); break;
    case SettingId::StatusBar: i = int(s.statusBar); break;
    case SettingId::Flip: i = s.flip ? 1 : 0; break;
    case SettingId::HoldAction: i = int(s.holdAction); break;
    case SettingId::LockOnSleep: i = s.lockOnSleep ? 1 : 0; break;
    case SettingId::Language: i = int(s.language); break;
    case SettingId::FullEvery: i = indexOf(kFullEvery, s.fullEvery); break;
    case SettingId::LongPress: i = indexOf(kLongPress, s.longPressMs); break;
    case SettingId::IdleMinutes: i = indexOf(kIdle, s.idleMinutes); break;
    case SettingId::DeepSleep: i = s.deepSleep ? 1 : 0; break;
    case SettingId::WakeRefresh: i = int(s.wakeRefresh); break;
    case SettingId::Count: break;
  }
  return i >= 0 && i < settingOptionCount(id) ? i : 0;
}

void setSettingIndex(Settings &s, SettingId id, int i) {
  if (i < 0 || i >= settingOptionCount(id)) return;
  switch (id) {
    case SettingId::SleepScreen: s.sleepScreen = SleepScreen(i); break;
    case SettingId::StatusBar: s.statusBar = StatusBarMode(i); break;
    case SettingId::Flip: s.flip = i == 1; break;
    case SettingId::HoldAction: s.holdAction = HoldAction(i); break;
    case SettingId::LockOnSleep: s.lockOnSleep = i == 1; break;
    case SettingId::Language: s.language = Lang(i); break;
    case SettingId::FullEvery: s.fullEvery = kFullEvery[i]; break;
    case SettingId::LongPress: s.longPressMs = kLongPress[i]; break;
    case SettingId::IdleMinutes: s.idleMinutes = kIdle[i]; break;
    case SettingId::DeepSleep: s.deepSleep = i == 1; break;
    case SettingId::WakeRefresh: s.wakeRefresh = WakeRefresh(i); break;
    case SettingId::Count: break;
  }
}

void cycleSetting(Settings &s, SettingId id) {
  const int n = settingOptionCount(id);
  if (n) setSettingIndex(s, id, (settingIndex(s, id) + 1) % n);
}

const char *settingKey(SettingId id) {
  switch (id) {
    case SettingId::SleepScreen: return "sleepScreen";
    case SettingId::StatusBar: return "statusBar";
    case SettingId::Flip: return "flip";
    case SettingId::HoldAction: return "holdAction";
    case SettingId::LockOnSleep: return "lockOnSleep";
    case SettingId::Language: return "language";
    case SettingId::FullEvery: return "fullEvery";
    case SettingId::LongPress: return "longPress";
    case SettingId::IdleMinutes: return "idleMinutes";
    case SettingId::DeepSleep: return "deepSleep";
    case SettingId::WakeRefresh: return "wakeRefresh";
    case SettingId::Count: break;
  }
  return "";
}

bool settingFromKey(const char *key, SettingId &out) {
  for (int i = 0; i < kSettingCount; i++) {
    if (strcmp(key, settingKey(SettingId(i))) == 0) {
      out = SettingId(i);
      return true;
    }
  }
  return false;
}

const char *settingLabel(SettingId id, S holdLabel) {
  switch (id) {
    case SettingId::SleepScreen: return tr(S::SetSleepScreen);
    case SettingId::StatusBar: return tr(S::SetStatusBar);
    case SettingId::Flip: return tr(S::SetFlip);
    case SettingId::HoldAction: return tr(holdLabel);
    case SettingId::LockOnSleep: return tr(S::SetLockOnSleep);
    case SettingId::Language: return tr(S::SetLanguage);
    case SettingId::FullEvery: return tr(S::SetFullRefresh);
    case SettingId::LongPress: return tr(S::SetHoldTime);
    case SettingId::IdleMinutes: return tr(S::SetIdleSleep);
    case SettingId::DeepSleep: return tr(S::SetSleepAfterPage);
    case SettingId::WakeRefresh: return tr(S::SetWakeRefresh);
    case SettingId::Count: break;
  }
  return "?";
}

void settingOptionText(SettingId id, int i, char *out, size_t n) {
  if (!n) return;
  out[0] = '\0';
  if (i < 0 || i >= settingOptionCount(id)) return;
  static const S kScreens[] = {S::ScreenBook, S::ScreenCover, S::ScreenPictures, S::ScreenPage};
  static const S kBars[] = {S::BarFull, S::BarMinimal, S::BarHidden};
  static const S kHolds[] = {S::HoldSleep, S::HoldBookmark, S::HoldLock, S::HoldOpenMenu};
  static const S kWakes[] = {S::WakeRewrite, S::WakeRetain, S::WakeFull};
  switch (id) {
    case SettingId::SleepScreen: snprintf(out, n, "%s", tr(kScreens[i])); break;
    case SettingId::StatusBar: snprintf(out, n, "%s", tr(kBars[i])); break;
    case SettingId::HoldAction: snprintf(out, n, "%s", tr(kHolds[i])); break;
    case SettingId::Flip:
    case SettingId::LockOnSleep:
    case SettingId::DeepSleep: snprintf(out, n, "%s", tr(i ? S::On : S::Off)); break;
    case SettingId::Language: snprintf(out, n, "%s", languageName(Lang(i))); break;
    case SettingId::FullEvery:
      if (kFullEvery[i]) {
        snprintf(out, n, tr(S::EveryN), unsigned(kFullEvery[i]));
      } else {
        snprintf(out, n, "%s", tr(S::ChaptersOnly));
      }
      break;
    case SettingId::LongPress: snprintf(out, n, "%u ms", unsigned(kLongPress[i])); break;
    case SettingId::IdleMinutes: snprintf(out, n, "%u min", unsigned(kIdle[i])); break;
    case SettingId::WakeRefresh: snprintf(out, n, "%s", tr(kWakes[i])); break;
    case SettingId::Count: break;
  }
}

void settingValue(const Settings &s, SettingId id, char *out, size_t n) {
  settingOptionText(id, settingIndex(s, id), out, n);
}
