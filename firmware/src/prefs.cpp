#include "prefs.h"

#include <Preferences.h>

#include "t5b_format.h"

namespace prefs {
namespace {
Preferences store;
}

void begin() { store.begin("reader", false); }

Settings loadSettings() {
  const Settings d;
  Settings s;
  s.fullEvery = store.getUChar("fullEvery", d.fullEvery);
  s.longPressMs = store.getUShort("longMs", d.longPressMs);
  s.idleMinutes = store.getUChar("idleMin", d.idleMinutes);
  s.deepSleep = store.getBool("deepSleep", d.deepSleep);
  s.wakeRefresh = WakeRefresh(store.getUChar("wakeRefresh", uint8_t(d.wakeRefresh)));
  sanitizeSettings(s);
  return s;
}

void saveSettings(const Settings &s) {
  store.putUChar("fullEvery", s.fullEvery);
  store.putUShort("longMs", s.longPressMs);
  store.putUChar("idleMin", s.idleMinutes);
  store.putBool("deepSleep", s.deepSleep);
  store.putUChar("wakeRefresh", uint8_t(s.wakeRefresh));
}

String lastBook() { return store.getString("lastBook", ""); }

void setLastBook(const char *path) {
  if (lastBook() != path) store.putString("lastBook", path);
}

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

}  // namespace prefs
