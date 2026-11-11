// "Wi-Fi transfer": upload, list and delete books from a browser.
//
// Joins your home network (entered on the page, or include/secrets.h's WIFI_SSID /
// WIFI_PASSWORD), or starts the reader's own network (WPA2, password kept in NVS).
// Serves the page from include/web_assets.h and a small JSON API; every API call
// needs the 4-digit session code shown on the e-ink screen. Wi-Fi is off
// whenever this mode isn't running.
#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "t5b_format.h"

namespace transfer {

struct Info {
  bool running = false;
  bool ownNetwork = true;  // reader's own network (AP), else home network (STA)
  char ssid[33] = "";
  char password[9] = "";  // own network only
  char address[40] = "";  // "192.168.4.1"
  char hostname[24] = "";  // "t5reader.local" on the home network
  char url[64] = "";       // page URL including the session code
  char code[5] = "";       // session code
  int clients = 0;         // devices joined to the own network
  char event[80] = "";     // last upload/delete, for the screen
  uint32_t lastActivityMs = 0;
};

// Starts Wi-Fi and the web server. batteryPct is reported to the page.
bool start(int batteryPct);
void stop();
void loop();  // call often while running

const Info &info();
// True once after anything shown on the screen changed (client joined, upload).
bool takeChanged();

// Joins the home Wi-Fi (entered on the page, else secrets.h). `ssid` gets its name
// ("" if none is set). False: none set, or not reachable (Wi-Fi is off again).
// Also used by the book server (bookserver.cpp).
bool joinHomeWifi(char *ssid, size_t ssidSize);

// A book written to `tmp` (uploaded, or downloaded from the book server) becomes
// `finalPath`. It is checked as the library checks it (Invalid: `why` says what is
// wrong, tmp is gone), then replaces a book of that name. The reading position and
// bookmarks of the replaced book move along in proportion: always (a restyle from
// the page), or only when the title is the same (SameTitle: a download, where the
// next part of a book replaces the last and must start at its own beginning).
enum class Install : uint8_t { Ok, Invalid, CannotStore };
enum class KeepPosition : uint8_t { Always, SameTitle };
Install installBook(const String &tmp, const String &finalPath, KeepPosition keep,
                    t5b::Header &h, const char *&why);

}  // namespace transfer
