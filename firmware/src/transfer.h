// "Wi-Fi transfer": upload, list and delete books from a browser.
//
// Joins your home network (entered on the page, or include/secrets.h's WIFI_SSID /
// WIFI_PASSWORD), or starts the reader's own network (WPA2, password kept in NVS).
// Serves the page from include/web_index.h and a small JSON API; every API call
// needs the 4-digit session code shown on the e-ink screen. Wi-Fi is off
// whenever this mode isn't running.
#pragma once

#include <stdint.h>

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

}  // namespace transfer
