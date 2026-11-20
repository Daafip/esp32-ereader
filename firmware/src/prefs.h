// Persistent state in NVS: settings, last opened book, reading position per
// book, device name, home Wi-Fi, book server, MQTT broker, statistics, lock and
// clock zone.
#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "reading_stats.h"
#include "settings.h"

namespace prefs {

void begin();

// Stored settings; anything not stored yet comes from `defaults`.
Settings loadSettings(const Settings &defaults);
void saveSettings(const Settings &s);

String lastBook();
void setLastBook(const char *path);

// Password of the reader's own Wi-Fi network (created once, then reused so
// laptops can remember the network). Empty if not set yet.
String apPassword();
void setApPassword(const char *password);

// Home Wi-Fi entered on the web page (takes precedence over secrets.h).
// The password is never sent back to the page.
String homeSsid();
String homePassword();
void setHomeWifi(const char *ssid, const char *password);  // empty ssid: forget

// The book server's address, entered on the page ("" = none; then secrets.h's
// BOOK_SERVER, see bookserver.h).
String bookServer();
void setBookServer(const char *address);  // empty: forget

// The MQTT broker for the reading position sync ("" = none; then secrets.h's
// MQTT_BROKER, see syncer.h): "host[:port]", a user and a password (both may be
// empty). The password goes back to the page only for a settings backup.
String mqttBroker();
String mqttUser();
String mqttPassword();
void setMqtt(const char *broker, const char *user, const char *password);  // empty broker: forget

// Shown as the library title ("" = default).
String deviceName();
void setDeviceName(const char *name);

// Page to resume a book at (0 if never opened).
uint32_t bookmark(uint32_t bookId);
void setBookmark(uint32_t bookId, uint32_t page);

ReadingStats loadStats();
void saveStats(const ReadingStats &st);

bool locked();
void setLocked(bool on);

int32_t timeZoneOffset();  // seconds east of UTC, from the phone
void setTimeZoneOffset(int32_t seconds);

uint32_t nextPicture();  // rotating index for the sleep pictures
void setNextPicture(uint32_t i);

// Everything above back to defaults.
void clearAll();

}  // namespace prefs
