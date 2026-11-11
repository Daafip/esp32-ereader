// The book server (tools/bookprep/src/bookprep/bookserver.py, the Home Assistant
// add-on in homeassistant/t5books): its address and its replies, parsed without
// Arduino so they can be unit-tested on the PC. The network side is
// src/bookserver.cpp.
//
// Replies are plain UTF-8 text: the line kProtocol, then one record per line with
// fields separated by tabs ("book<TAB>id<TAB>title<TAB>author<TAB>file").
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace catalog {

constexpr uint16_t kDefaultPort = 8484;          // the add-on's port
constexpr const char *kProtocol = "T5BOOKS\t1";  // first line of every reply

struct Address {
  char host[64] = "";
  uint16_t port = kDefaultPort;
  char base[48] = "";  // path in front of /api, without a trailing slash ("" mostly)
};

// "192.168.68.101", "host:8484", "http://host:8484/", "http://host/prefix". Without
// a scheme the port is the add-on's (8484), with http:// it is 80. https:// and
// anything else is refused: the reader speaks plain HTTP on the home network.
bool parseAddress(const char *text, Address &out);

// `in` percent-encoded for a query value (A-Z a-z 0-9 - . _ ~ kept). False if `out`
// was too small (output cut at a whole escape, still terminated).
bool urlEncode(const char *in, char *out, size_t outSize);

// Splits a reply line at tabs in place (a trailing \r or \n goes first). Returns the
// number of fields, at most maxFields (the last one then keeps any further tabs).
int splitFields(char *line, char **fields, int maxFields);

// "HTTP/1.0 200 OK" -> 200. False if the line is not a status line.
bool parseStatus(const char *line, int &code);
// "Content-Length: 1234" (name in any case) -> 1234. False for any other header.
bool parseContentLength(const char *line, uint32_t &length);

// Whether a book title names that part of a book: ends in " (171–340)" with an en
// dash, as bookprep's part_title() writes it.
bool isPartTitle(const char *title, uint32_t first, uint32_t last);

// Bytes a download may take: the free space plus the book it replaces, less a margin
// for the file system's own blocks: at least 1 (nothing fits), or 0 if the free space
// is unknown (freeBytes < 0).
constexpr uint32_t kRoomMargin = 32u << 10;
uint32_t downloadRoom(int64_t freeBytes, uint32_t replacedBytes);

}  // namespace catalog
