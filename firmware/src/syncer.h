// Reading position sync over MQTT (docs/progress-sync.md). The broker (Mosquitto in
// Home Assistant) keeps one retained record per book, t5reader/progress/<key>. A
// session joins the home Wi-Fi if it isn't on, reads the records of the books on the
// reader, notes the places other readers went to (asked about when the book is open),
// publishes the books this reader moved on in, and leaves. What to do with a book:
// lib/core/src/position_sync.*; this file does the network and the files.
//
// The broker is entered on the reader's page (Settings) or in include/secrets.h as
// MQTT_BROKER / MQTT_USER / MQTT_PASSWORD. Plain MQTT 3.1.1 over TCP (port 1883), no
// TLS, as the book server's plain HTTP: the home network only.
//
// Per book on the card: /books/.sync/<stem>.txt (possync::BookState).
#pragma once

#include <Arduino.h>
#include <stdint.h>

#include <vector>

#include "position_sync.h"

namespace syncer {

constexpr const char *kSyncDir = "/books/.sync";

// "host[:port]" from the page, else secrets.h's MQTT_BROKER; "" if none.
String broker();
bool configured();

// This reader in the records: 8 hex digits from its MAC.
const char *deviceId();

// Starts joining the home Wi-Fi without waiting, unless Wi-Fi is on: a sync right
// after a panel refresh has the join run during it.
void beginJoin();
// Wi-Fi off again if beginJoin() or run() switched it on (before sleep, or before the
// book server or Wi-Fi transfer take the Wi-Fi over).
void endJoin();

enum class Result : uint8_t {
  Ok,
  NoBroker,     // none set
  BadAddress,   // the broker's address can't be parsed
  NoWifi,       // no home network set
  WifiFailed,   // not joined (out of reach)
  Unreachable,  // no TCP connection, or no answer
  Refused,      // the broker refused the login (user, password)
  Failed,       // anything else (no card, a broken reply)
};
struct Report {
  Result result = Result::Ok;
  uint16_t sent = 0;      // records published
  uint16_t received = 0;  // places other readers went to (questions now open)
  char detail[72] = "";   // the broker or the network, for messages
};
// One session for every book on the reader: joins the home Wi-Fi if it isn't on
// (waiting at most joinMs), and switches it off again if it switched it on. `idle` is
// called while waiting (the app reads the buttons there; nothing is swallowed).
Report run(uint32_t joinMs, void (*idle)());

// A book's state on the card (defaults if there is none).
void load(const String &stem, possync::BookState &out);
bool save(const String &stem, const possync::BookState &s);
void forget(const String &stem);  // the book was deleted

// Where a book's copy lies in the whole book: a part (from its state) or the whole
// book, whose contents are `contents` (its own TOC start pages). The layout points into
// `s` or `contents`: keep both while using it.
possync::Layout layout(const possync::BookState &s, uint32_t pages,
                       const std::vector<uint32_t> &contents);
// Whether a book can be synced: not a part whose place in the whole book is unknown
// (made before this firmware, or by `bookprep --first-page`).
bool syncable(const possync::BookState &s, const char *title, uint32_t pages);

// A new copy of a book arrived (an upload, a download): all of it, or a part (first
// page 0-based, the whole book's pages and contents, empty if not known).
void installedWhole(const String &stem);
void installedPart(const String &stem, uint32_t first, uint32_t wholePages,
                   const std::vector<uint32_t> &starts);

}  // namespace syncer
