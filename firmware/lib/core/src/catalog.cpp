#include "catalog.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace catalog {

namespace {

bool startsWithNoCase(const char *s, const char *prefix) {
  for (; *prefix; s++, prefix++) {
    if (tolower(uint8_t(*s)) != tolower(uint8_t(*prefix))) return false;
  }
  return true;
}

bool copy(char *out, size_t size, const char *from, size_t len) {
  if (len >= size) return false;
  memcpy(out, from, len);
  out[len] = '\0';
  return true;
}

}  // namespace

bool parseAddress(const char *text, Address &out) {
  out = Address();
  while (*text == ' ') text++;
  size_t len = strlen(text);
  while (len && text[len - 1] == ' ') len--;
  const char *end = text + len;
  const char *p = text;
  if (startsWithNoCase(p, "http://")) {
    p += 7;
    out.port = 80;
  } else if (strstr(p, "://")) {
    return false;  // https, ftp, ...
  }
  const char *hostEnd = p;
  while (hostEnd < end && *hostEnd != ':' && *hostEnd != '/') hostEnd++;
  if (hostEnd == p || !copy(out.host, sizeof(out.host), p, size_t(hostEnd - p))) return false;
  for (const char *c = out.host; *c; c++) {
    if (!isalnum(uint8_t(*c)) && *c != '.' && *c != '-' && *c != '_') return false;
  }
  p = hostEnd;
  if (p < end && *p == ':') {
    p++;
    if (p == end || !isdigit(uint8_t(*p))) return false;
    unsigned long port = 0;
    while (p < end && isdigit(uint8_t(*p))) {
      port = port * 10 + unsigned(*p++ - '0');
      if (port > 65535) return false;
    }
    if (port == 0) return false;
    out.port = uint16_t(port);
  }
  if (p < end && *p != '/') return false;
  const char *pathEnd = end;
  while (pathEnd > p && pathEnd[-1] == '/') pathEnd--;  // no trailing slash
  for (const char *c = p; c < pathEnd; c++) {
    if (uint8_t(*c) <= ' ' || *c == '?' || *c == '#') return false;
  }
  return copy(out.base, sizeof(out.base), p, size_t(pathEnd - p));
}

bool urlEncode(const char *in, char *out, size_t outSize) {
  static const char kHex[] = "0123456789ABCDEF";
  if (!outSize) return false;
  size_t n = 0;
  for (; *in; in++) {
    const uint8_t c = uint8_t(*in);
    if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
      if (n + 1 >= outSize) break;
      out[n++] = char(c);
    } else {
      if (n + 3 >= outSize) break;
      out[n++] = '%';
      out[n++] = kHex[c >> 4];
      out[n++] = kHex[c & 15];
    }
  }
  out[n] = '\0';
  return *in == '\0';
}

int splitFields(char *line, char **fields, int maxFields) {
  size_t len = strlen(line);
  while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
  if (maxFields <= 0) return 0;
  int n = 0;
  fields[n++] = line;
  for (char *p = line; *p && n < maxFields; p++) {
    if (*p == '\t') {
      *p = '\0';
      fields[n++] = p + 1;
    }
  }
  return n;
}

bool parseStatus(const char *line, int &code) {
  if (strncmp(line, "HTTP/1.", 7) != 0 || !isdigit(uint8_t(line[7])) || line[8] != ' ') {
    return false;
  }
  const char *p = line + 9;
  if (!isdigit(uint8_t(p[0])) || !isdigit(uint8_t(p[1])) || !isdigit(uint8_t(p[2]))) return false;
  if (p[3] != '\0' && p[3] != ' ' && p[3] != '\r') return false;
  code = (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
  return true;
}

bool parseContentLength(const char *line, uint32_t &length) {
  if (!startsWithNoCase(line, "content-length:")) return false;
  const char *p = line + 15;
  while (*p == ' ' || *p == '\t') p++;
  if (!isdigit(uint8_t(*p))) return false;
  uint64_t value = 0;
  while (isdigit(uint8_t(*p))) {
    value = value * 10 + uint64_t(*p++ - '0');
    if (value > 0xFFFFFFFFull) return false;
  }
  length = uint32_t(value);
  return true;
}

bool isPartTitle(const char *title, uint32_t first, uint32_t last) {
  char suffix[40];
  snprintf(suffix, sizeof(suffix), " (%u\xE2\x80\x93%u)", unsigned(first), unsigned(last));
  const size_t n = strlen(title), s = strlen(suffix);
  return n > s && strcmp(title + n - s, suffix) == 0;
}

uint32_t downloadRoom(int64_t freeBytes, uint32_t replacedBytes) {
  if (freeBytes < 0) return 0;
  const int64_t room = freeBytes + int64_t(replacedBytes) - int64_t(kRoomMargin);
  if (room <= 0) return 1;  // known, and nothing fits ("0" would mean unknown)
  return room > 0xFFFFFFFFll ? 0xFFFFFFFFu : uint32_t(room);
}

}  // namespace catalog
