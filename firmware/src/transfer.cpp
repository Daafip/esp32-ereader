#include "transfer.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <SD.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_app_format.h>

#include <vector>

#include "board.h"
#include "board_tag.h"
#include "prefs.h"
#include "storage.h"
#include "t5b_format.h"
#include "web_assets.h"
#include "web_util.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

namespace transfer {
namespace {

constexpr const char *kHostname = "t5reader";
constexpr uint32_t kJoinTimeoutMs = 12000;
constexpr char kQuote = '"';

WebServer server(80);
Info state;
bool changed = false;
bool routesAdded = false;
int battery = -1;

// What can be uploaded, where it goes on the card, and how big it may be.
// book: the typeset .t5b; source/meta: the original EPUB/TXT and the converter
// settings, kept for "Restyle"; font: reading fonts added from the browser.
struct Kind {
  const char *name;
  const char *dir;
  const char *const *exts;
  size_t extCount;
  uint32_t maxBytes;
};
const char *const kBookExts[] = {".t5b"};
const char *const kSourceExts[] = {".epub", ".txt"};
const char *const kMetaExts[] = {".json"};
const char *const kFontExts[] = {".ttf", ".otf", ".woff", ".woff2"};
constexpr const char *kSourceDir = "/books/.src";
constexpr const char *kFontDir = "/fonts";
// 64 MB, or 4096 pages where pages are big (T5-4.7": 64 KB each).
constexpr uint32_t kBookMaxBytes =
    uint32_t(kPageBytes) * 4096u > (64u << 20) ? uint32_t(kPageBytes) * 4096u : 64u << 20;
const Kind kKinds[] = {
    {"book", "/books", kBookExts, 1, kBookMaxBytes},
    {"source", kSourceDir, kSourceExts, 2, 64u << 20},
    {"meta", kSourceDir, kMetaExts, 1, 64u << 10},
    {"font", kFontDir, kFontExts, 4, 8u << 20},
};

// The board tag in this firmware; a Wi-Fi update must carry the same one. Images
// from before 0.9.0 have none and are accepted (the chip is still checked).
constexpr size_t kTagPrefixLen = 15;  // "T5READER-BOARD:"
__attribute__((used)) const char kBoardTag[] = "T5READER-BOARD:" BOARD_NAME;
static_assert(sizeof("T5READER-BOARD:") - 1 == kTagPrefixLen, "prefix length");
BoardTagScanner fwTag(kBoardTag, kTagPrefixLen);

const Kind *findKind(const String &name) {
  for (const Kind &k : kKinds) {
    if (name == k.name) return &k;
  }
  return nullptr;
}

String stemOf(const String &name) {
  const int dot = name.lastIndexOf('.');
  return dot > 0 ? name.substring(0, dot) : name;
}

const char *mimeFor(const String &name) {
  if (name.endsWith(".json")) return "application/json";
  if (name.endsWith(".epub")) return "application/epub+zip";
  if (name.endsWith(".txt")) return "text/plain";
  return "application/octet-stream";
}

// Upload in progress (one at a time; WebServer is synchronous).
const Kind *upKind = nullptr;
fs::File upFile;
String upTmp;
String upFinal;
uint8_t upBuf[4096];
size_t upFill = 0;
int upStatus = 200;
String upError;
char upTitle[t5b::kTitleBytes + 1];
uint32_t upPages = 0;
uint32_t upId = 0;  // t5b::bookId of the uploaded book (raw header title, like the library)

// Firmware update (OTA into the other app slot), then restart.
int fwStatus = 200;
String fwError;
uint32_t restartAtMs = 0;

void touch(const char *event = nullptr) {
  state.lastActivityMs = millis();
  if (event) {
    strlcpy(state.event, event, sizeof(state.event));
    changed = true;
  }
}

// --- tiny JSON writer -------------------------------------------------------------

void addKey(String &out, const char *key) {
  if (out.length() > 1 && out[out.length() - 1] != '{' && out[out.length() - 1] != '[') out += ',';
  out += kQuote;
  out += key;
  out += kQuote;
  out += ':';
}

void addString(String &out, const char *key, const char *value) {
  char esc[160];
  jsonEscape(value, esc, sizeof(esc));
  addKey(out, key);
  out += kQuote;
  out += esc;
  out += kQuote;
}

void addNumber(String &out, const char *key, uint64_t value) {
  char num[24];
  snprintf(num, sizeof(num), "%llu", static_cast<unsigned long long>(value));
  addKey(out, key);
  out += num;
}

void addInt(String &out, const char *key, int value) {
  addKey(out, key);
  out += value;
}

void sendJson(int code, const String &body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

void sendError(int code, const char *message) {
  String out = "{";
  addString(out, "error", message);
  out += '}';
  sendJson(code, out);
}

bool authorized() { return server.arg("k") == state.code; }

const char *baseName(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

// --- handlers ---------------------------------------------------------------------

// The page, the converter script, fonts and hyphenation data (web_assets.h).
bool serveAsset(const String &uri) {
  for (const WebAsset &a : kWebAssets) {
    if (uri != a.path) continue;
    touch();
    if (a.gzip) server.sendHeader("Content-Encoding", "gzip");
    server.sendHeader("Cache-Control", uri == "/" ? "no-cache" : "max-age=86400");
    server.send_P(200, a.mime, reinterpret_cast<const char *>(a.data), a.len);
    return true;
  }
  return false;
}

void handleIndex() { serveAsset("/"); }

void handleInfo() {
  if (!authorized()) return sendError(403, "wrong code");
  touch();
  std::vector<storage::BookEntry> books;
  storage::listBooks(books, 200);
  String out;
  out.reserve(160 + books.size() * 200);
  out = "{";
  addString(out, "version", FW_VERSION);
  addString(out, "board", BOARD_NAME);
  // This reader's screen: the page typesets books for it (see bookprep.js, screens).
  addKey(out, "screen");
  out += '{';
  addString(out, "id", kScreenId);
  addInt(out, "width", kPageW);
  addInt(out, "height", kPageH);
  addInt(out, "statusBar", kStatusBarH);
  addInt(out, "size", kDefaultFontPx);
  addInt(out, "margin", kDefaultMarginPx);
  out += '}';
  addInt(out, "battery", battery);
  addNumber(out, "total", SD.totalBytes());
  addNumber(out, "free", SD.totalBytes() - SD.usedBytes());
  addKey(out, "books");
  out += '[';
  for (const storage::BookEntry &b : books) {
    if (out[out.length() - 1] != '[') out += ',';
    out += '{';
    addString(out, "file", baseName(b.path.c_str()));
    addString(out, "title", b.title.c_str());
    addString(out, "author", b.author.c_str());
    addNumber(out, "pages", b.pages);
    addNumber(out, "size", b.size);
    const uint32_t mark = prefs::bookmark(b.id);
    addInt(out, "progress", b.pages > 1 ? int(uint64_t(mark) * 100 / (b.pages - 1)) : 0);
    // the original kept for "Restyle", if any
    const String stem = stemOf(baseName(b.path.c_str()));
    String source;
    for (const char *ext : kSourceExts) {
      if (SD.exists(String(kSourceDir) + "/" + stem + ext)) {
        source = stem + ext;
        break;
      }
    }
    if (source.length()) {
      addString(out, "source", source.c_str());
    } else {
      addKey(out, "source");
      out += "null";
    }
    out += '}';
  }
  out += ']';
  addKey(out, "fonts");
  out += '[';
  fs::File dir = SD.open(kFontDir);
  if (dir && dir.isDirectory()) {
    for (fs::File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      if (f.isDirectory() || !isSafeFileName(f.name(), kFontExts, 4)) continue;
      if (out[out.length() - 1] != '[') out += ',';
      out += '{';
      addString(out, "name", f.name());
      addNumber(out, "size", f.size());
      out += '}';
    }
  }
  out += "]}";
  sendJson(200, out);
}

void failUpload(int status, const char *message) {
  if (upStatus == 200) {
    upStatus = status;
    upError = message;
  }
  if (upFile) upFile.close();
  if (upTmp.length()) SD.remove(upTmp);
}

bool flushUpload() {
  if (!upFill) return true;
  const bool ok = upFile.write(upBuf, upFill) == upFill;
  upFill = 0;
  return ok;
}

// Validates the finished temp file as a book for this screen; fills upTitle.
bool validateUpload() {
  fs::File f = SD.open(upTmp, FILE_READ);
  uint8_t head[t5b::kHeaderSize];
  t5b::Header h;
  t5b::Error err = t5b::Error::TooShort;
  if (f && f.read(head, sizeof(head)) == sizeof(head)) {
    err = t5b::parseHeader(head, sizeof(head), h);
    if (err == t5b::Error::None) err = t5b::validate(h, kPageW, kPageH, uint32_t(f.size()));
  }
  if (f) f.close();
  if (err != t5b::Error::None) {
    failUpload(400, t5b::errorText(err));
    return false;
  }
  strlcpy(upTitle, h.title[0] ? h.title : baseName(upFinal.c_str()), sizeof(upTitle));
  upPages = h.pageCount;
  upId = t5b::bookId(h.title, h.pageCount);
  return true;
}

void handleUploadData() {
  HTTPUpload &up = server.upload();
  switch (up.status) {
    case UPLOAD_FILE_START: {
      upStatus = 200;
      upError = "";
      upFill = 0;
      upTmp = "";
      upTitle[0] = '\0';
      upPages = 0;
      if (!authorized()) return failUpload(403, "wrong code");
      upKind = findKind(server.hasArg("kind") ? server.arg("kind") : String("book"));
      if (!upKind) return failUpload(400, "unknown kind");
      String name = up.filename;
      const int slash = max(name.lastIndexOf('/'), name.lastIndexOf('\\'));
      if (slash >= 0) name = name.substring(slash + 1);
      if (!isSafeFileName(name.c_str(), upKind->exts, upKind->extCount)) {
        return failUpload(400, "file type not allowed here");
      }
      SD.mkdir(storage::kBooksDir);
      SD.mkdir(upKind->dir);
      upFinal = String(upKind->dir) + "/" + name;
      upTmp = upFinal + ".part";
      SD.remove(upTmp);
      upFile = SD.open(upTmp, FILE_WRITE);
      if (!upFile) return failUpload(500, "cannot write to the SD card");
      Serial.printf("upload start: %s\n", name.c_str());
      touch();
      break;
    }
    case UPLOAD_FILE_WRITE: {
      if (upStatus != 200) return;
      if (up.totalSize > upKind->maxBytes) return failUpload(413, "file too large");
      size_t offset = 0;
      while (offset < up.currentSize) {
        const size_t n = min(sizeof(upBuf) - upFill, up.currentSize - offset);
        memcpy(upBuf + upFill, up.buf + offset, n);
        upFill += n;
        offset += n;
        if (upFill == sizeof(upBuf) && !flushUpload()) return failUpload(507, "SD card full?");
      }
      state.lastActivityMs = millis();
      break;
    }
    case UPLOAD_FILE_END: {
      if (upStatus != 200) return;
      if (!flushUpload()) return failUpload(507, "SD card full?");
      upFile.close();
      const bool isBook = upKind == &kKinds[0];
      if (isBook && !validateUpload()) return;
      // Replacing a book (restyled): carry the reading position over in proportion.
      uint32_t oldMark = 0, oldPages = 0;
      if (isBook) {
        fs::File old = SD.open(upFinal, FILE_READ);
        uint8_t head[t5b::kHeaderSize];
        t5b::Header oh;
        if (old && old.read(head, sizeof(head)) == sizeof(head) &&
            t5b::parseHeader(head, sizeof(head), oh) == t5b::Error::None) {
          oldPages = oh.pageCount;
          oldMark = prefs::bookmark(t5b::bookId(oh.title, oh.pageCount));
        }
        if (old) old.close();
      }
      SD.remove(upFinal);
      if (!SD.rename(upTmp, upFinal)) return failUpload(500, "could not store the file");
      if (isBook && oldMark && oldPages > 1 && upPages > 1) {
        const uint32_t mark = uint32_t((uint64_t(oldMark) * (upPages - 1) + (oldPages - 1) / 2) / (oldPages - 1));
        prefs::setBookmark(upId, mark);
      }
      if (!isBook) {
        state.lastActivityMs = millis();
        break;
      }
      char event[80];
      snprintf(event, sizeof(event), "Received %s", upTitle);
      Serial.printf("upload done: %s (%u bytes)\n", upFinal.c_str(), unsigned(up.totalSize));
      touch(event);
      break;
    }
    case UPLOAD_FILE_ABORTED:
      failUpload(400, "upload aborted");
      break;
  }
}

void handleUploadDone() {
  if (upStatus != 200) return sendError(upStatus, upError.c_str());
  String out = "{";
  addInt(out, "ok", 1);
  addString(out, "title", upTitle);
  out += '}';
  sendJson(200, out);
}

void failFirmware(int status, const char *message) {
  if (fwStatus == 200) {
    fwStatus = status;
    fwError = message;
    Serial.printf("firmware update failed: %s\n", message);
  }
  Update.abort();
}

void handleFirmwareData() {
  HTTPUpload &up = server.upload();
  switch (up.status) {
    case UPLOAD_FILE_START:
      fwStatus = 200;
      fwError = "";
      if (!authorized()) return failFirmware(403, "wrong code");
      fwTag.reset();
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) return failFirmware(500, Update.errorString());
      touch("Updating firmware...");
      break;
    case UPLOAD_FILE_WRITE:
      if (fwStatus != 200) return;
      if (Update.progress() == 0 && up.currentSize >= sizeof(esp_image_header_t)) {
        // The image of the other board (ESP32 vs ESP32-S3) would only fail at the end.
        const auto *h = reinterpret_cast<const esp_image_header_t *>(up.buf);
        if (h->magic == ESP_IMAGE_HEADER_MAGIC && h->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
          return failFirmware(400, "firmware for the other board (wrong chip)");
        }
      }
      fwTag.feed(up.buf, up.currentSize);
      if (Update.write(up.buf, up.currentSize) != up.currentSize) {
        // e.g. "Magic byte is wrong": not an app image (the merged .bin from fw-dist)
        return failFirmware(400, Update.errorString());
      }
      state.lastActivityMs = millis();
      break;
    case UPLOAD_FILE_END:
      if (fwStatus != 200) return;
      if (fwTag.found() && strcmp(fwTag.name(), BOARD_NAME) != 0) {
        char why[96];
        snprintf(why, sizeof(why), "firmware for another board (%s, this is %s)", fwTag.name(),
                 BOARD_NAME);
        return failFirmware(400, why);
      }
      if (!Update.end(true)) return failFirmware(400, Update.errorString());
      Serial.printf("firmware update ok (%u bytes), restarting\n", unsigned(up.totalSize));
      touch("Firmware updated, restarting");
      break;
    case UPLOAD_FILE_ABORTED:
      failFirmware(400, "upload aborted");
      break;
  }
}

void handleFirmwareDone() {
  if (fwStatus != 200) return sendError(fwStatus, fwError.c_str());
  sendJson(200, "{}");
  restartAtMs = millis() + 1000;  // let the response go out first
}

void handleDelete() {
  if (!authorized()) return sendError(403, "wrong code");
  const String name = server.arg("file");
  if (!isSafeBookName(name.c_str())) return sendError(400, "bad file name");
  const String path = String(storage::kBooksDir) + "/" + name;
  if (!SD.exists(path)) return sendError(404, "no such book");
  if (!SD.remove(path)) return sendError(500, "could not delete");
  const String stem = String(kSourceDir) + "/" + stemOf(name);
  for (const char *ext : {".epub", ".txt", ".json"}) SD.remove(stem + ext);
  char event[80];
  snprintf(event, sizeof(event), "Deleted %s", name.c_str());
  touch(event);
  sendJson(200, "{}");
}

void handleDeleteFont() {
  if (!authorized()) return sendError(403, "wrong code");
  const String name = server.arg("name");
  if (!isSafeFileName(name.c_str(), kFontExts, 4)) return sendError(400, "bad file name");
  if (!SD.remove(String(kFontDir) + "/" + name)) return sendError(404, "no such font");
  touch();
  sendJson(200, "{}");
}

// GET /file?kind=source|meta|font&name=...: what "Restyle" and user fonts need.
void handleFile() {
  if (!authorized()) return sendError(403, "wrong code");
  const Kind *kind = findKind(server.arg("kind"));
  const String name = server.arg("name");
  if (!kind || kind == &kKinds[0] || !isSafeFileName(name.c_str(), kind->exts, kind->extCount)) {
    return sendError(400, "bad request");
  }
  fs::File f = SD.open(String(kind->dir) + "/" + name, FILE_READ);
  if (!f || f.isDirectory()) return sendError(404, "not found");
  touch();
  server.streamFile(f, mimeFor(name));
  f.close();
}

void handleNotFound() {
  if (serveAsset(server.uri())) return;
  if (server.method() == HTTP_GET && !server.uri().startsWith("/api/")) {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
    return;
  }
  sendError(404, "not found");
}

void addRoutes() {
  if (routesAdded) return;
  server.on("/", HTTP_GET, handleIndex);
  server.on("/api/info", HTTP_GET, handleInfo);
  server.on("/api/upload", HTTP_POST, handleUploadDone, handleUploadData);
  server.on("/api/book", HTTP_DELETE, handleDelete);
  server.on("/api/font", HTTP_DELETE, handleDeleteFont);
  server.on("/file", HTTP_GET, handleFile);
  server.on("/api/firmware", HTTP_POST, handleFirmwareDone, handleFirmwareData);
  server.onNotFound(handleNotFound);
  routesAdded = true;
}

bool joinHomeNetwork() {
#ifdef WIFI_SSID
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(kHostname);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < kJoinTimeoutMs) delay(100);
  if (WiFi.status() == WL_CONNECTED) {
    state.ownNetwork = false;
    strlcpy(state.ssid, WIFI_SSID, sizeof(state.ssid));
    strlcpy(state.address, WiFi.localIP().toString().c_str(), sizeof(state.address));
    if (MDNS.begin(kHostname)) {
      MDNS.addService("http", "tcp", 80);
      snprintf(state.hostname, sizeof(state.hostname), "%s.local", kHostname);
    }
    return true;
  }
  Serial.println("home Wi-Fi not reachable, starting own network");
  WiFi.disconnect(true);
#endif
  return false;
}

void startOwnNetwork() {
  state.ownNetwork = true;
  const uint64_t mac = ESP.getEfuseMac();
  char ssid[16];
  makeSsid(uint32_t(mac >> 32) ^ uint32_t(mac), ssid);
  strlcpy(state.ssid, ssid, sizeof(state.ssid));
  String pass = prefs::apPassword();
  if (pass.length() != 8) {
    char fresh[9];
    makePassword(esp_random(), esp_random(), fresh);
    prefs::setApPassword(fresh);
    pass = fresh;
  }
  strlcpy(state.password, pass.c_str(), sizeof(state.password));
  WiFi.mode(WIFI_AP);
  WiFi.softAP(state.ssid, state.password, 6, 0, 3);
  strlcpy(state.address, WiFi.softAPIP().toString().c_str(), sizeof(state.address));
}

}  // namespace

bool start(int batteryPct) {
  battery = batteryPct;
  state = Info();
  makeCode(esp_random(), state.code);
  if (!joinHomeNetwork()) startOwnNetwork();
  snprintf(state.url, sizeof(state.url), "http://%s/?k=%s", state.address, state.code);
  addRoutes();
  server.begin();
  state.running = true;
  changed = true;
  touch();
  Serial.printf("Wi-Fi transfer: %s network %s, %s\n", state.ownNetwork ? "own" : "home",
                state.ssid, state.url);
  return true;
}

void stop() {
  if (!state.running) return;
  server.stop();
  if (!state.ownNetwork) MDNS.end();
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  state.running = false;
  Serial.println("Wi-Fi transfer stopped");
}

void loop() {
  if (!state.running) return;
  server.handleClient();
  if (restartAtMs && int32_t(millis() - restartAtMs) >= 0) {
    stop();
    ESP.restart();
  }
  if (state.ownNetwork) {
    const int n = WiFi.softAPgetStationNum();
    if (n != state.clients) {
      state.clients = n;
      changed = true;
      touch();
    }
  }
}

const Info &info() { return state; }

bool takeChanged() {
  const bool c = changed;
  changed = false;
  return c;
}

}  // namespace transfer
