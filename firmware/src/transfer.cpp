#include "transfer.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_app_format.h>
#include <esp_ota_ops.h>
#include <sys/time.h>
#include <time.h>

#include <vector>

#include "board.h"
#include "board_tag.h"
#include "catalog.h"
#include "bookmarks.h"
#include "display.h"
#include "i18n.h"
#include "prefs.h"
#include "rtc_clock.h"
#include "ui.h"
#include "reading_stats.h"
#include "settings.h"
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
const char *const kPictureExts[] = {".bin"};
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
    {"sleep", storage::kPicturesDir, kPictureExts, 1, uint32_t(kPageBytes)},  // at most a page
};
const Kind &kBookKind = kKinds[0];
const Kind &kPictureKind = kKinds[4];

// Where a kind's files are: pictures for the screen shown now (storage::picturesDir()).
String kindDir(const Kind &k) { return &k == &kPictureKind ? storage::picturesDir() : String(k.dir); }

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

String stemOf(const String &name) { return storage::stemOf(name); }

// "/books/Folder/x.t5b" -> "Folder/x.t5b": how the page names a book.
String relPath(const String &path) { return path.substring(strlen(storage::kBooksDir) + 1); }

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

// A book named by the page ("x.t5b" or "Folder/x.t5b") -> full path, or "" if unsafe.
String bookPathArg(const char *arg = "file") {
  const String rel = server.arg(arg);
  return isSafeBookPath(rel.c_str()) ? String(storage::kBooksDir) + "/" + rel : String();
}

// Opens a book named by the page and reads its header; false sends the error.
bool openBookArg(fs::File &f, t5b::Header &h, String &path) {
  path = bookPathArg();
  if (!path.length()) {
    sendError(400, "bad file name");
    return false;
  }
  f = storage::files().open(path, FILE_READ);
  if (!f || storage::checkBook(f, h) != t5b::Error::None) {
    if (f) f.close();
    sendError(404, "no such book");
    return false;
  }
  return true;
}

// Where an uploaded book goes: the `folder` the page names, else where a book of
// that name already is (a restyle or a second upload replaces it), else /books.
String bookDirFor(const String &name) {
  if (server.hasArg("folder")) {
    const String folder = server.arg("folder");
    if (!folder.length()) return storage::kBooksDir;
    return isSafeFolderName(folder.c_str()) ? String(storage::kBooksDir) + "/" + folder : String();
  }
  return storage::bookDirFor(name);
}

// --- handlers ---------------------------------------------------------------------

// The page, the converter script, fonts and hyphenation data (web_assets.h).
// Browsers keep them but ask each time (ETag): after an update they get the new ones.
bool serveAsset(const String &uri) {
  for (const WebAsset &a : kWebAssets) {
    if (uri != a.path) continue;
    touch();
    server.sendHeader("Cache-Control", "no-cache");
    server.sendHeader("ETag", a.etag);
    if (server.header("If-None-Match") == a.etag) {
      server.send(304);
      return true;
    }
    if (a.gzip) server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, a.mime, reinterpret_cast<const char *>(a.data), a.len);
    return true;
  }
  return false;
}

void handleIndex() { serveAsset("/"); }

void addBool(String &out, const char *key, bool value) {
  addKey(out, key);
  out += value ? "true" : "false";
}

// A second app slot to update into (FireBeetle 7.5" without a card: none).
bool hasUpdateSlot() { return esp_ota_get_next_update_partition(nullptr) != nullptr; }

void addStats(String &out) {
  const ReadingStats st = prefs::loadStats();
  const uint32_t now = uint32_t(time(nullptr));
  const int32_t tz = prefs::timeZoneOffset();
  addKey(out, "stats");
  out += '{';
  addNumber(out, "pages", st.pages);
  addNumber(out, "seconds", st.seconds);
  addNumber(out, "daysRead", st.daysRead);
  addNumber(out, "streak", currentStreak(st, now, tz));
  addNumber(out, "bestStreak", st.bestStreak);
  addNumber(out, "todayPages", todayPages(st, now, tz));
  addNumber(out, "todaySeconds", todaySeconds(st, now, tz));
  addBool(out, "clock", now >= kClockValid);
  out += '}';
}

void handleInfo() {
  if (!authorized()) return sendError(403, "wrong code");
  touch();
  std::vector<storage::BookEntry> books;
  storage::listAllBooks(books);
  String out;
  out.reserve(600 + books.size() * 240);
  out = "{";
  addString(out, "version", FW_VERSION);
  addString(out, "board", BOARD_NAME);
  addString(out, "lang", language() == Lang::Nl ? "nl" : "en");
  addString(out, "name", prefs::deviceName().c_str());
  addString(out, "homeWifi", prefs::homeSsid().c_str());  // never the password
  addString(out, "bookServer", prefs::bookServer().c_str());
  addBool(out, "locked", prefs::locked());
  // This reader's screen now: the page typesets books for it (see bookprep.js,
  // screens). The 7.5" turns (Settings -> Orientation): portrait or landscape.
  const ScreenSpec &sc = display::screen();
  addKey(out, "screen");
  out += '{';
  addString(out, "id", sc.id);
  addInt(out, "width", sc.w);
  addInt(out, "height", sc.h);
  addInt(out, "statusBar", sc.statusBar);
  addInt(out, "size", sc.fontPx);
  addInt(out, "margin", sc.marginPx);
  out += '}';
  addInt(out, "battery", battery);
  addNumber(out, "total", storage::totalBytes());
  addNumber(out, "free", uint64_t(max<int64_t>(storage::freeBytes(), 0)));
  // No second app slot (books in the flash): "Update firmware" can't work, so the
  // page says to update over USB instead.
  addBool(out, "ota", hasUpdateSlot());
  addStats(out);
  addKey(out, "books");
  out += '[';
  for (const storage::BookEntry &b : books) {
    if (out[out.length() - 1] != '[') out += ',';
    out += '{';
    addString(out, "file", relPath(b.path).c_str());
    addString(out, "folder", b.folder.c_str());
    addString(out, "title", b.title.c_str());
    addString(out, "author", b.author.c_str());
    addNumber(out, "pages", b.pages);
    addNumber(out, "size", b.size);
    addString(out, "screen", kScreens[b.screen].id);  // the 7.5": maybe the other one
    const uint32_t mark = prefs::bookmark(b.id);
    addNumber(out, "position", mark);
    addInt(out, "progress", b.pages > 1 ? int(uint64_t(mark) * 100 / (b.pages - 1)) : 0);
    addBool(out, "cover", b.hasCover);
    PageList marks;
    const String stem = stemOf(b.path);
    storage::readMarks(stem, b.pages, marks);
    addInt(out, "marks", marks.count());
    // the original kept for "Restyle", if any
    String source;
    for (const char *ext : kSourceExts) {
      if (storage::files().exists(String(kSourceDir) + "/" + stem + ext)) {
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
  fs::File dir = storage::files().open(kFontDir);
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
  out += ']';
  addKey(out, "pictures");
  out += '[';
  for (int i = 0, n = storage::pictureCount(storage::PictureShape::Any); i < n; i++) {
    if (out[out.length() - 1] != '[') out += ',';
    out += kQuote;
    out += baseName(storage::picturePath(storage::PictureShape::Any, i).c_str());
    out += kQuote;
  }
  out += "]}";
  sendJson(200, out);
}

constexpr const char *kCardFull = "not enough space on the reader";

// Why isSafeFileName() refused an upload's name, for the page.
const char *badNameReason(String name, const Kind &k) {
  static char tooLong[48];
  if (name.length() > kMaxFileName) {
    snprintf(tooLong, sizeof(tooLong), "file name too long (%u bytes at most)",
             unsigned(kMaxFileName));
    return tooLong;
  }
  name.toLowerCase();
  for (size_t i = 0; i < k.extCount; i++) {
    if (name.endsWith(k.exts[i])) return "file name not allowed (leading dot or \\/:*?\"<>|)";
  }
  return "file type not allowed here";
}

void failUpload(int status, const char *message) {
  if (upStatus == 200) {
    upStatus = status;
    upError = message;
  }
  if (upFile) upFile.close();
  if (upTmp.length()) storage::files().remove(upTmp);
}

bool flushUpload() {
  if (!upFill) return true;
  const bool ok = upFile.write(upBuf, upFill) == upFill;
  upFill = 0;
  return ok;
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
      if (!authorized()) return failUpload(403, "wrong code");
      upKind = findKind(server.hasArg("kind") ? server.arg("kind") : String("book"));
      if (!upKind) return failUpload(400, "unknown kind");
      String name = up.filename;
      const int slash = max(name.lastIndexOf('/'), name.lastIndexOf('\\'));
      if (slash >= 0) name = name.substring(slash + 1);
      if (!isSafeFileName(name.c_str(), upKind->exts, upKind->extCount)) {
        return failUpload(400, badNameReason(name, *upKind));
      }
      // Refuse what can't fit at once instead of after a card-full write. The written
      // copy goes next to a book it replaces, so the whole file must fit. (The body is
      // still read to the end before the reply: WebServer has no early answer.)
      const int64_t room = storage::freeBytes();
      if (room >= 0 && int64_t(server.clientContentLength()) > room) {
        return failUpload(507, kCardFull);
      }
      String dir = kindDir(*upKind);
      if (upKind == &kBookKind) {
        dir = bookDirFor(name);
        if (!dir.length()) return failUpload(400, "bad folder name");
      }
      storage::files().mkdir(storage::kBooksDir);
      storage::files().mkdir(upKind->dir);
      storage::files().mkdir(dir);
      upFinal = dir + "/" + name;
      upTmp = upFinal + ".part";
      storage::files().remove(upTmp);
      upFile = storage::files().open(upTmp, FILE_WRITE);
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
        if (upFill == sizeof(upBuf) && !flushUpload()) return failUpload(507, kCardFull);
      }
      state.lastActivityMs = millis();
      break;
    }
    case UPLOAD_FILE_END: {
      if (upStatus != 200) return;
      if (!flushUpload()) return failUpload(507, kCardFull);
      upFile.close();
      {  // the last bytes sit in a stdio buffer until close(), which can't report a full card
        fs::File f = storage::files().open(upTmp, FILE_READ);
        const size_t stored = f ? f.size() : 0;
        if (f) f.close();
        if (stored != up.totalSize) return failUpload(507, kCardFull);
      }
      const bool isBook = upKind == &kBookKind;
      if (upKind == &kPictureKind && up.totalSize != size_t(kPageBytes) &&
          up.totalSize != size_t(kSquareBytes)) {
        char why[80];
        snprintf(why, sizeof(why), "a sleep picture is %ux%u (%u bytes) or %ux%u (%u bytes)",
                 unsigned(display::screen().w), unsigned(display::screen().h),
                 unsigned(kPageBytes), unsigned(kSquareSide),
                 unsigned(kSquareSide), unsigned(kSquareBytes));
        return failUpload(400, why);
      }
      if (!isBook) {
        storage::files().remove(upFinal);
        if (!storage::files().rename(upTmp, upFinal)) {
          return failUpload(500, "could not store the file");
        }
        state.lastActivityMs = millis();
        break;
      }
      // A book for one of the board's screens (the 7.5" has two); a restyle replaces
      // the old one, its reading position moving along.
      t5b::Header h;
      const char *why = "";
      switch (installBook(upTmp, upFinal, KeepPosition::Always, h, why)) {
        case Install::Ok: break;
        case Install::Invalid: return failUpload(400, why);
        case Install::CannotStore: return failUpload(500, why);
      }
      strlcpy(upTitle, h.title[0] ? h.title : baseName(upFinal.c_str()), sizeof(upTitle));
      char event[80];
      snprintf(event, sizeof(event), tr(S::Received), upTitle);
      Serial.printf("upload done: %s (%u bytes)\n", upFinal.c_str(), unsigned(up.totalSize));
      touch(event);
      break;
    }
    case UPLOAD_FILE_ABORTED:
      failUpload(400, "upload aborted");
      break;
  }
}

// The reply carries the card's free space, so the page's storage line stays current.
void handleUploadDone() {
  if (upStatus != 200 && upStatus != 507) return sendError(upStatus, upError.c_str());
  String out = "{";
  if (upStatus == 200) {
    addInt(out, "ok", 1);
    addString(out, "title", upTitle);
  } else {
    addString(out, "error", upError.c_str());
  }
  const int64_t room = storage::freeBytes();
  if (room >= 0) addNumber(out, "free", uint64_t(room));
  out += '}';
  sendJson(upStatus, out);
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
      if (!hasUpdateSlot()) {
        return failFirmware(400, "no room for an update over Wi-Fi: flash over USB");
      }
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) return failFirmware(500, Update.errorString());
      touch(tr(S::UpdatingFirmware));
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
      touch(tr(S::FirmwareUpdated));
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
  const String path = bookPathArg();
  if (!path.length()) return sendError(400, "bad file name");
  if (!storage::files().exists(path)) return sendError(404, "no such book");
  if (!storage::files().remove(path)) return sendError(500, "could not delete");
  const String stem = stemOf(path);
  for (const char *ext : {".epub", ".txt", ".json"}) {
    storage::files().remove(String(kSourceDir) + "/" + stem + ext);
  }
  storage::writeMarks(stem, PageList());
  const String dir = path.substring(0, path.lastIndexOf('/'));
  if (dir != storage::kBooksDir) storage::files().rmdir(dir);  // only succeeds when empty
  char event[80];
  snprintf(event, sizeof(event), tr(S::Deleted), baseName(path.c_str()));
  touch(event);
  sendJson(200, "{}");
}

// POST /api/move?file=Folder/x.t5b&folder=Other ("" = /books). Folders are
// made on demand and removed when they run empty.
void handleMove() {
  if (!authorized()) return sendError(403, "wrong code");
  const String from = bookPathArg();
  const String folder = server.arg("folder");
  if (!from.length() || (folder.length() && !isSafeFolderName(folder.c_str()))) {
    return sendError(400, "bad name");
  }
  const String dir = folder.length() ? String(storage::kBooksDir) + "/" + folder : storage::kBooksDir;
  const String to = dir + "/" + baseName(from.c_str());
  if (to == from) return sendJson(200, "{}");
  if (!storage::files().exists(from)) return sendError(404, "no such book");
  if (storage::files().exists(to)) return sendError(409, "a book with that name is already there");
  storage::files().mkdir(dir);
  if (!storage::files().rename(from, to)) return sendError(500, "could not move");
  if (prefs::lastBook() == from) prefs::setLastBook(to.c_str());
  const String oldDir = from.substring(0, from.lastIndexOf('/'));
  if (oldDir != storage::kBooksDir) storage::files().rmdir(oldDir);
  char event[80];
  snprintf(event, sizeof(event), tr(S::Moved), baseName(from.c_str()));
  touch(event);
  sendJson(200, "{}");
}

// Bookmarks of one book. GET: {"pages":[...]}; POST page=&op=add|remove.
void handleMarks() {
  if (!authorized()) return sendError(403, "wrong code");
  fs::File f;
  t5b::Header h;
  String path;
  if (!openBookArg(f, h, path)) return;
  f.close();
  PageList marks;
  storage::readMarks(stemOf(path), h.pageCount, marks);
  if (server.method() == HTTP_POST) {
    const long page = server.arg("page").toInt();
    if (page < 0 || uint32_t(page) >= h.pageCount) return sendError(400, "bad page");
    if (server.arg("op") == "remove") {
      marks.remove(uint32_t(page));
    } else {
      marks.add(uint32_t(page));
    }
    if (!storage::writeMarks(stemOf(path), marks)) return sendError(500, "could not save");
  }
  touch();
  String out = "{";
  addKey(out, "pages");
  out += '[';
  for (int i = 0; i < marks.count(); i++) {
    if (i) out += ',';
    out += String(marks.at(i));
  }
  out += "]}";
  sendJson(200, out);
}

// POST /api/position?file=&page=: the reader opens this book at that page next.
void handlePosition() {
  if (!authorized()) return sendError(403, "wrong code");
  fs::File f;
  t5b::Header h;
  String path;
  if (!openBookArg(f, h, path)) return;
  f.close();
  const long page = server.arg("page").toInt();
  if (page < 0 || uint32_t(page) >= h.pageCount) return sendError(400, "bad page");
  prefs::setBookmark(t5b::bookId(h.title, h.pageCount), uint32_t(page));
  prefs::setLastBook(path.c_str());
  touch();
  sendJson(200, "{}");
}

// GET /api/page?file=&n=: one page bitmap (the .t5b page layout) for the preview.
// A compressed page goes out as it is stored, a zlib stream: Content-Encoding
// "deflate", which the browser inflates.
void handlePage() {
  if (!authorized()) return sendError(403, "wrong code");
  fs::File f;
  t5b::Header h;
  String path;
  if (!openBookArg(f, h, path)) return;
  const long n = server.arg("n").toInt();
  storage::FileReader reader(f);
  uint32_t offset = 0, len = 0;
  if (n < 0 || t5b::pageSpan(reader, h, uint32_t(n), offset, len) != t5b::Error::None ||
      !f.seek(offset)) {
    f.close();
    return sendError(400, "bad page");
  }
  touch();
  static uint8_t buf[2048];
  if (h.compressed()) server.sendHeader("Content-Encoding", "deflate");
  server.setContentLength(len);
  server.send(200, "application/octet-stream", "");
  for (uint32_t left = len; left;) {
    const size_t chunk = left < sizeof(buf) ? left : sizeof(buf);
    if (f.read(buf, chunk) != chunk) break;
    server.sendContent(reinterpret_cast<const char *>(buf), chunk);
    left -= chunk;
  }
  f.close();
}

// Device settings for the page: GET lists them (labels and options in the
// reader's language), POST key=&value=<option index> changes one.
void handleSettings() {
  if (!authorized()) return sendError(403, "wrong code");
  Settings st = prefs::loadSettings(Settings());
  if (server.method() == HTTP_POST) {
    SettingId id;
    const int value = server.arg("value").toInt();
    if (!settingFromKey(server.arg("key").c_str(), id) || value < 0 ||
        value >= settingOptionCount(id)) {
      return sendError(400, "bad setting");
    }
    setSettingIndex(st, id, value);
    prefs::saveSettings(st);
    if (id == SettingId::Language) setLanguage(st.language);
    touch(tr(S::SettingsChanged));
  }
  const S holdLabel = ui::holdSettingLabel();
  String out = "{";
  addKey(out, "settings");
  out += '[';
  for (int i = 0; i < kSettingCount; i++) {
    const SettingId id = SettingId(i);
    if (i) out += ',';
    out += '{';
    addString(out, "key", settingKey(id));
    addString(out, "label", settingLabel(id, holdLabel));
    addInt(out, "value", settingIndex(st, id));
    addKey(out, "options");
    out += '[';
    for (int o = 0; o < settingOptionCount(id); o++) {
      char text[48];
      settingOptionText(id, o, text, sizeof(text));
      char esc[96];
      jsonEscape(text, esc, sizeof(esc));
      if (o) out += ',';
      out += kQuote;
      out += esc;
      out += kQuote;
    }
    out += "]}";
  }
  out += "]}";
  sendJson(200, out);
}

// POST name=: the library title.
void handleName() {
  if (!authorized()) return sendError(403, "wrong code");
  const String name = server.arg("name");
  if (name.length() > 32) return sendError(400, "name too long");
  for (size_t i = 0; i < name.length(); i++) {
    if (uint8_t(name[i]) < 0x20) return sendError(400, "bad name");
  }
  prefs::setDeviceName(name.c_str());
  touch();
  sendJson(200, "{}");
}

// POST ssid=&password=: home Wi-Fi for the next Wi-Fi transfer (DELETE: forget).
// The password is stored on the reader only and never sent back.
void handleHomeWifi() {
  if (!authorized()) return sendError(403, "wrong code");
  if (server.method() == HTTP_DELETE) {
    prefs::setHomeWifi("", "");
  } else {
    const String ssid = server.arg("ssid");
    const String pass = server.arg("password");
    if (!ssid.length() || ssid.length() > 32 || pass.length() > 63) {
      return sendError(400, "network name 1-32 characters, password at most 63");
    }
    prefs::setHomeWifi(ssid.c_str(), pass.c_str());
  }
  touch();
  sendJson(200, "{}");
}

// POST address=: the book server (Menu -> Book server; bookserver.h). DELETE: forget.
void handleBookServer() {
  if (!authorized()) return sendError(403, "wrong code");
  if (server.method() == HTTP_DELETE) {
    prefs::setBookServer("");
  } else {
    String address = server.arg("address");
    address.trim();
    catalog::Address parsed;
    if (address.length() > 96 || !catalog::parseAddress(address.c_str(), parsed)) {
      return sendError(400, "not an address like http://192.168.1.10:8484");
    }
    prefs::setBookServer(address.c_str());
  }
  touch();
  sendJson(200, "{}");
}

// POST epoch=&tz=: the phone's clock (for the reading statistics' days).
void handleTime() {
  if (!authorized()) return sendError(403, "wrong code");
  const uint32_t epoch = uint32_t(strtoul(server.arg("epoch").c_str(), nullptr, 10));
  const long tz = server.arg("tz").toInt();
  if (epoch < kClockValid || tz < -14 * 3600 || tz > 14 * 3600) return sendError(400, "bad time");
  timeval tv = {time_t(epoch), 0};
  settimeofday(&tv, nullptr);
  rtcclock::save(epoch);
  prefs::setTimeZoneOffset(int32_t(tz));
  sendJson(200, "{}");
}

void handleDeletePicture() {
  if (!authorized()) return sendError(403, "wrong code");
  const String name = server.arg("name");
  if (!isSafeFileName(name.c_str(), kPictureExts, 1)) return sendError(400, "bad file name");
  if (!storage::files().remove(storage::picturesDir() + "/" + name)) {
    return sendError(404, "no such picture");
  }
  touch();
  sendJson(200, "{}");
}

// POST confirm=erase: books, originals, bookmarks, pictures, fonts and every
// setting gone; then a restart.
void handleReset() {
  if (!authorized()) return sendError(403, "wrong code");
  if (server.arg("confirm") != "erase") return sendError(400, "confirm=erase missing");
  storage::eraseAll();
  prefs::clearAll();
  sendJson(200, "{}");
  restartAtMs = millis() + 1000;
}

void handleDeleteFont() {
  if (!authorized()) return sendError(403, "wrong code");
  const String name = server.arg("name");
  if (!isSafeFileName(name.c_str(), kFontExts, 4)) return sendError(400, "bad file name");
  if (!storage::files().remove(String(kFontDir) + "/" + name)) {
    return sendError(404, "no such font");
  }
  touch();
  sendJson(200, "{}");
}

// GET /file?kind=source|meta|font&name=...: what "Restyle" and user fonts need.
void handleFile() {
  if (!authorized()) return sendError(403, "wrong code");
  const Kind *kind = findKind(server.arg("kind"));
  const String name = server.arg("name");
  if (!kind || kind == &kBookKind || !isSafeFileName(name.c_str(), kind->exts, kind->extCount)) {
    return sendError(400, "bad request");
  }
  fs::File f = storage::files().open(kindDir(*kind) + "/" + name, FILE_READ);
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
  static const char *kHeaders[] = {"If-None-Match"};
  server.collectHeaders(kHeaders, 1);
  server.on("/", HTTP_GET, handleIndex);
  server.on("/api/info", HTTP_GET, handleInfo);
  server.on("/api/upload", HTTP_POST, handleUploadDone, handleUploadData);
  server.on("/api/book", HTTP_DELETE, handleDelete);
  server.on("/api/font", HTTP_DELETE, handleDeleteFont);
  server.on("/api/move", HTTP_POST, handleMove);
  server.on("/api/marks", HTTP_GET, handleMarks);
  server.on("/api/marks", HTTP_POST, handleMarks);
  server.on("/api/position", HTTP_POST, handlePosition);
  server.on("/api/page", HTTP_GET, handlePage);
  server.on("/api/settings", HTTP_GET, handleSettings);
  server.on("/api/settings", HTTP_POST, handleSettings);
  server.on("/api/name", HTTP_POST, handleName);
  server.on("/api/wifi", HTTP_POST, handleHomeWifi);
  server.on("/api/wifi", HTTP_DELETE, handleHomeWifi);
  server.on("/api/bookserver", HTTP_POST, handleBookServer);
  server.on("/api/bookserver", HTTP_DELETE, handleBookServer);
  server.on("/api/time", HTTP_POST, handleTime);
  server.on("/api/picture", HTTP_DELETE, handleDeletePicture);
  server.on("/api/reset", HTTP_POST, handleReset);
  server.on("/file", HTTP_GET, handleFile);
  server.on("/api/firmware", HTTP_POST, handleFirmwareDone, handleFirmwareData);
  server.onNotFound(handleNotFound);
  routesAdded = true;
}

bool joinHomeNetwork() {
  char ssid[33];
  if (!joinHomeWifi(ssid, sizeof(ssid))) {
    if (ssid[0]) Serial.printf("home Wi-Fi %s not reachable, starting own network\n", ssid);
    return false;
  }
  state.ownNetwork = false;
  strlcpy(state.ssid, ssid, sizeof(state.ssid));
  strlcpy(state.address, WiFi.localIP().toString().c_str(), sizeof(state.address));
  if (MDNS.begin(kHostname)) {
    MDNS.addService("http", "tcp", 80);
    snprintf(state.hostname, sizeof(state.hostname), "%s.local", kHostname);
  }
  return true;
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

// Home Wi-Fi: entered on the page (NVS), else include/secrets.h, else none.
bool joinHomeWifi(char *ssidOut, size_t ssidSize) {
  String ssid = prefs::homeSsid();
  String pass = prefs::homePassword();
#ifdef WIFI_SSID
  if (!ssid.length()) {
    ssid = WIFI_SSID;
    pass = WIFI_PASSWORD;
  }
#endif
  strlcpy(ssidOut, ssid.c_str(), ssidSize);
  if (!ssid.length()) return false;
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(kHostname);
  WiFi.begin(ssid.c_str(), pass.c_str());
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < kJoinTimeoutMs) delay(100);
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  return false;
}

Install installBook(const String &tmp, const String &finalPath, KeepPosition keep,
                    t5b::Header &h, const char *&why) {
  fs::File f = storage::files().open(tmp, FILE_READ);
  const t5b::Error err = f ? storage::checkBook(f, h) : t5b::Error::TooShort;
  if (f) f.close();
  if (err != t5b::Error::None) {
    storage::files().remove(tmp);
    why = t5b::errorText(err);
    return Install::Invalid;
  }
  // The book it replaces, if any: its position, bookmarks and title.
  uint32_t oldMark = 0, oldPages = 0;
  t5b::Header oh;
  {
    fs::File old = storage::files().open(finalPath, FILE_READ);
    uint8_t head[t5b::kHeaderSize];
    if (old && old.read(head, sizeof(head)) == sizeof(head) &&
        t5b::parseHeader(head, sizeof(head), oh) == t5b::Error::None) {
      oldPages = oh.pageCount;
      oldMark = prefs::bookmark(t5b::bookId(oh.title, oh.pageCount));
    }
    if (old) old.close();
  }
  storage::files().remove(finalPath);
  if (!storage::files().rename(tmp, finalPath)) {
    storage::files().remove(tmp);
    why = "could not store the file";
    return Install::CannotStore;
  }
  const bool sameBook = keep == KeepPosition::Always || strcmp(oh.title, h.title) == 0;
  if (oldPages && oldPages != h.pageCount && sameBook) {  // restyled: positions move along
    // raw header title, like the library
    const uint32_t id = t5b::bookId(h.title, h.pageCount);
    if (oldMark) prefs::setBookmark(id, remapPage(oldMark, oldPages, h.pageCount));
    PageList marks;
    storage::readMarks(stemOf(finalPath), oldPages, marks);
    marks.remap(oldPages, h.pageCount);
    storage::writeMarks(stemOf(finalPath), marks);
  } else if (oldPages && !sameBook) {
    storage::writeMarks(stemOf(finalPath), PageList());  // another part: its own marks
  }
  return Install::Ok;
}

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
      Serial.printf("Wi-Fi transfer: %d device(s) on the network\n", n);
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
