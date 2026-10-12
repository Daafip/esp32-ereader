// Helpers for the Wi-Fi transfer web server (pure, unit-tested on the host).
#pragma once

#include <stddef.h>
#include <stdint.h>

// Appends `in` as a JSON string body (no surrounding quotes): escapes " \ and
// control characters. Returns false if `out` was too small (output truncated).
bool jsonEscape(const char *in, char *out, size_t outSize);

// A plain file name we accept for upload/delete: 1-64 bytes, ends in ".t5b"
// (any case), no path separators, no leading dot, printable characters only.
bool isSafeBookName(const char *name);

// Wi-Fi join QR payload: WIFI:T:WPA;S:<ssid>;P:<password>;; with \ ; , : " escaped.
bool wifiQrPayload(const char *ssid, const char *password, char *out, size_t outSize);

// Reader network credentials from random numbers: SSID "T5-Reader-XXXX" from the
// chip id, an 8-character password without look-alike characters, a 4-digit code.
void makeSsid(uint32_t chipId, char out[16]);
void makePassword(uint32_t r1, uint32_t r2, char out[9]);
void makeCode(uint32_t r, char out[5]);
