// Helpers for the Wi-Fi transfer web server (pure, unit-tested on the host).
#pragma once

#include <stddef.h>
#include <stdint.h>

// Appends `in` as a JSON string body (no surrounding quotes): escapes " \ and
// control characters. Returns false if `out` was too small (output truncated).
bool jsonEscape(const char *in, char *out, size_t outSize);

// A plain file name we accept for upload/delete/download: 1-64 bytes, ends in one
// of `exts` (any case, e.g. ".epub") with something before it, no path
// separators, no leading dot, no control or shell-special characters.
bool isSafeFileName(const char *name, const char *const *exts, size_t extCount);

// isSafeFileName for ".t5b".
bool isSafeBookName(const char *name);

// A library folder (one level below /books): 1-32 bytes, no leading dot, no path
// separators, no control or shell-special characters.
bool isSafeFolderName(const char *name);

// A book relative to /books: "name.t5b" or "folder/name.t5b".
bool isSafeBookPath(const char *path);

// Wi-Fi join QR payload: WIFI:T:WPA;S:<ssid>;P:<password>;; with \ ; , : " escaped.
bool wifiQrPayload(const char *ssid, const char *password, char *out, size_t outSize);

// Reader network credentials from random numbers: SSID "T5-Reader-XXXX" from the
// chip id, an 8-character password without look-alike characters, a 4-digit code.
void makeSsid(uint32_t chipId, char out[16]);
void makePassword(uint32_t r1, uint32_t r2, char out[9]);
void makeCode(uint32_t r, char out[5]);
