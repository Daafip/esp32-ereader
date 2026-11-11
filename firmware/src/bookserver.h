// "Book server": books from a Home Assistant add-on (homeassistant/t5books, which
// runs tools/bookprep/src/bookprep/bookserver.py), typeset for this reader on the
// server and downloaded over the home Wi-Fi, without a phone.
//
// The server's address is entered on the reader's page (Settings) or in
// include/secrets.h as BOOK_SERVER. Requests are plain HTTP/1.0 GETs; replies are
// parsed by lib/core/src/catalog.h. One request at a time; Wi-Fi is on only while
// the app is in the book server's screens.
#pragma once

#include <Arduino.h>
#include <stdint.h>

#include <vector>

namespace bookserver {

// The configured address ("" if none).
String address();

enum class Join : uint8_t { Ok, NoWifiSet, NoServerSet, BadAddress, WifiFailed };
// Joins the home Wi-Fi. `ssid` gets the network's name (for messages).
Join connect(char *ssid, size_t ssidSize);
void disconnect();
bool connected();
// "192.168.68.101:8484", for messages.
const char *host();

// One record of a list: a profile (id, name), a book (id, title, author, file on
// the reader) or an author (id, name, number of books in `detail`).
struct Row {
  char id[24] = "";
  char title[96] = "";
  char detail[64] = "";
  char file[68] = "";
};

// Why the last call failed: a short text (the server's own words, "no answer",
// "HTTP 404", ...).
const char *error();

bool profiles(const char *screen, std::vector<Row> &out);
// /api/books or /api/authors with `query` ("sort=new", "author=<id>"): `count` rows
// from `offset`, and how many there are in all.
bool list(const char *what, const char *query, uint32_t offset, uint32_t count, uint32_t &total,
          std::vector<Row> &out);

struct Part {
  uint32_t first = 0, last = 0, bytes = 0;  // pages, 1-based
};
struct Prepared {
  enum class State : uint8_t { Busy, Ready, Failed } state = State::Busy;
  int percent = 0;  // Busy: how far the typesetting is
  int ahead = 0;    // Busy: conversions before this one
  uint32_t pages = 0, bytes = 0;  // Ready: the whole book
  char file[68] = "";             // Ready: its name on the reader
  std::vector<Part> parts;        // Ready and bigger than `room`: parts that fit
};
// Asks for a book typeset with a profile; the server starts if it hasn't yet. `room`:
// bytes the reader has for it (0: unknown). False: no answer (error()); a failed
// conversion is State::Failed with the reason in error().
bool prepare(const char *profile, const char *book, uint32_t room, Prepared &out);

// Downloads the book (first/last 0: whole) into `path`; `progress` is called as it
// arrives. False: error() says why, and `path` is removed.
using Progress = void (*)(uint32_t done, uint32_t total, void *ctx);
bool download(const char *profile, const char *book, uint32_t first, uint32_t last,
              const String &path, Progress progress, void *ctx);

}  // namespace bookserver
