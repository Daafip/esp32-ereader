#include "web_util.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

bool jsonEscape(const char *in, char *out, size_t outSize) {
  if (outSize == 0) return false;
  size_t n = 0;
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(in); *p; ++p) {
    char esc[8];
    const char *piece;
    size_t len;
    if (*p == '"' || *p == '\\') {
      esc[0] = '\\';
      esc[1] = char(*p);
      esc[2] = '\0';
      piece = esc;
      len = 2;
    } else if (*p < 0x20) {
      snprintf(esc, sizeof(esc), "\\u%04x", unsigned(*p));
      piece = esc;
      len = 6;
    } else {
      esc[0] = char(*p);
      esc[1] = '\0';
      piece = esc;
      len = 1;
    }
    if (n + len >= outSize) {
      out[n] = '\0';
      return false;
    }
    memcpy(out + n, piece, len);
    n += len;
  }
  out[n] = '\0';
  return true;
}

bool isSafeFileName(const char *name, const char *const *exts, size_t extCount) {
  const size_t n = strlen(name);
  if (n == 0 || n > 64 || name[0] == '.') return false;
  bool extOk = false;
  for (size_t i = 0; i < extCount && !extOk; i++) {
    const size_t e = strlen(exts[i]);
    extOk = n > e && strcasecmp(name + n - e, exts[i]) == 0;
  }
  if (!extOk) return false;
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = static_cast<unsigned char>(name[i]);
    if (c < 0x20 || c == 0x7F || c == '/' || c == '\\' || c == ':' || c == '"' || c == '*' ||
        c == '?' || c == '<' || c == '>' || c == '|') {
      return false;
    }
  }
  return true;
}

bool isSafeBookName(const char *name) {
  static const char *const kT5b[] = {".t5b"};
  return isSafeFileName(name, kT5b, 1);
}

namespace {
const char kSpecial[] = {'\\', ';', ',', ':', '"', '\0'};  // \ ; , : "

size_t appendEscaped(const char *s, char *out, size_t n, size_t cap) {
  for (; *s && n + 2 < cap; ++s) {
    if (strchr(kSpecial, *s)) out[n++] = '\\';
    out[n++] = *s;
  }
  return n;
}
}  // namespace

bool wifiQrPayload(const char *ssid, const char *password, char *out, size_t outSize) {
  // worst case: every character escaped
  if (outSize < 16 + 2 * (strlen(ssid) + strlen(password))) return false;
  size_t n = 0;
  n += size_t(snprintf(out, outSize, "WIFI:T:WPA;S:"));
  n = appendEscaped(ssid, out, n, outSize);
  memcpy(out + n, ";P:", 3);
  n += 3;
  n = appendEscaped(password, out, n, outSize);
  memcpy(out + n, ";;", 3);  // includes the terminator
  return true;
}

void makeSsid(uint32_t chipId, char out[16]) {
  snprintf(out, 16, "T5-Reader-%04X", unsigned(chipId & 0xFFFF));
}

void makePassword(uint32_t r1, uint32_t r2, char out[9]) {
  static const char kAlphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";  // no i l o 0 1
  const uint32_t len = sizeof(kAlphabet) - 1;
  uint64_t r = (uint64_t(r1) << 32) | r2;
  for (int i = 0; i < 8; i++) {
    out[i] = kAlphabet[r % len];
    r /= len;
  }
  out[8] = '\0';
}

void makeCode(uint32_t r, char out[5]) { snprintf(out, 5, "%04u", unsigned(r % 10000)); }
