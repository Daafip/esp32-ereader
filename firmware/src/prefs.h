// Persistent state in NVS: settings, last opened book, one bookmark per book.
#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "settings.h"

namespace prefs {

void begin();

Settings loadSettings();
void saveSettings(const Settings &s);

String lastBook();
void setLastBook(const char *path);

// Password of the reader's own Wi-Fi network (created once, then reused so
// laptops can remember the network). Empty if not set yet.
String apPassword();
void setApPassword(const char *password);

// Page to resume a book at (0 if never opened).
uint32_t bookmark(uint32_t bookId);
void setBookmark(uint32_t bookId, uint32_t page);

}  // namespace prefs
