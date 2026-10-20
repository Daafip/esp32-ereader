#include "prefs.h"

#include <Preferences.h>

#include "t5b_format.h"

namespace prefs {
namespace {
Preferences store;

String settingNvsKey(SettingId id) { return String("s_") + settingKey(id); }
}  // namespace

void begin() { store.begin("reader", false); }

Settings loadSettings(const Settings &defaults) {
  Settings s = defaults;
  for (int i = 0; i < kSettingCount; i++) {
    const SettingId id = SettingId(i);
    setSettingIndex(s, id, store.getUChar(settingNvsKey(id).c_str(), uint8_t(settingIndex(defaults, id))));
  }
  return s;
}

void saveSettings(const Settings &s) {
  for (int i = 0; i < kSettingCount; i++) {
    const SettingId id = SettingId(i);
    store.putUChar(settingNvsKey(id).c_str(), uint8_t(settingIndex(s, id)));  // NVS skips unchanged values
  }
}

String lastBook() { return store.getString("lastBook", ""); }

void setLastBook(const char *path) {
  if (lastBook() != path) store.putString("lastBook", path);
}

String apPassword() { return store.getString("apPass", ""); }
void setApPassword(const char *password) { store.putString("apPass", password); }

String homeSsid() { return store.getString("homeSsid", ""); }
String homePassword() { return store.getString("homePass", ""); }

void setHomeWifi(const char *ssid, const char *password) {
  if (!ssid || !*ssid) {
    store.remove("homeSsid");
    store.remove("homePass");
    return;
  }
  store.putString("homeSsid", ssid);
  store.putString("homePass", password ? password : "");
}

String deviceName() { return store.getString("name", ""); }
void setDeviceName(const char *name) { store.putString("name", name); }

uint32_t bookmark(uint32_t bookId) {
  char key[10];
  t5b::bookmarkKey(bookId, key);
  return store.getULong(key, 0);
}

void setBookmark(uint32_t bookId, uint32_t page) {
  char key[10];
  t5b::bookmarkKey(bookId, key);
  store.putULong(key, page);
}

ReadingStats loadStats() {
  ReadingStats st{};
  if (store.getBytesLength("stats") == sizeof(st)) store.getBytes("stats", &st, sizeof(st));
  return st;
}

void saveStats(const ReadingStats &st) { store.putBytes("stats", &st, sizeof(st)); }

bool locked() { return store.getBool("locked", false); }

void setLocked(bool on) {
  if (locked() != on) store.putBool("locked", on);
}

int32_t timeZoneOffset() { return store.getInt("tz", 0); }

void setTimeZoneOffset(int32_t seconds) {
  if (timeZoneOffset() != seconds) store.putInt("tz", seconds);
}

uint32_t nextPicture() { return store.getULong("picture", 0); }
void setNextPicture(uint32_t i) { store.putULong("picture", i); }

void clearAll() { store.clear(); }

}  // namespace prefs
