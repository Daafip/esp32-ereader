#include "bookserver.h"

#include <ESPmDNS.h>
#include <WiFi.h>

#include "catalog.h"
#include "i18n.h"
#include "prefs.h"
#include "storage.h"
#include "transfer.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

namespace bookserver {
namespace {

constexpr uint32_t kConnectMs = 5000;
constexpr uint32_t kReadMs = 15000;  // per line or chunk: a slow server, a weak signal
constexpr size_t kLineBytes = 400;   // a reply line: 5 fields of up to 90 bytes and tabs

catalog::Address addr;
bool joined = false;
bool mdnsStarted = false;
char hostText[72] = "";
char lastError[96] = "";

void setError(const char *text) {
  strlcpy(lastError, text, sizeof(lastError));
  Serial.printf("book server: %s\n", lastError);
}

// Reads one line (without \n) into buf; false at the end or after a timeout.
bool readLine(WiFiClient &c, char *buf, size_t size, uint32_t *left = nullptr) {
  size_t n = 0;
  uint32_t last = millis();
  for (;;) {
    if (left && *left == 0) break;
    if (!c.available()) {
      if (!c.connected() && !c.available()) break;
      if (millis() - last > kReadMs) return false;
      delay(2);
      continue;
    }
    const int ch = c.read();
    if (ch < 0) continue;
    last = millis();
    if (left) (*left)--;
    if (ch == '\n') {
      buf[n] = '\0';
      return true;
    }
    if (n + 1 < size) buf[n++] = char(ch);
  }
  buf[n] = '\0';
  return n > 0;
}

// The server's IP: an address, a name from DNS, or a .local name over mDNS.
bool resolve(IPAddress &ip) {
  if (ip.fromString(addr.host)) return true;
  const size_t n = strlen(addr.host);
  if (n > 6 && strcasecmp(addr.host + n - 6, ".local") == 0) {
    if (!mdnsStarted) mdnsStarted = MDNS.begin("t5reader");
    String name(addr.host);
    name.remove(name.length() - 6);
    ip = MDNS.queryHost(name, 3000);
    return ip != IPAddress();
  }
  return WiFi.hostByName(addr.host, ip) == 1;
}

// Sends GET base + target and reads the status line and headers. True for 200 with
// `length` from Content-Length (0 if none). Another status: false, and the reply's
// "error" line (if any) in error().
bool get(WiFiClient &c, const String &target, uint32_t &length) {
  IPAddress ip;
  if (!resolve(ip)) {
    setError(tr(S::NoAnswer));
    return false;
  }
  if (!c.connect(ip, addr.port, kConnectMs)) {
    setError(tr(S::NoAnswer));
    return false;
  }
  c.printf("GET %s%s HTTP/1.0\r\nHost: %s:%u\r\nConnection: close\r\n\r\n", addr.base,
           target.c_str(), addr.host, unsigned(addr.port));
  char line[kLineBytes];
  int code = 0;
  if (!readLine(c, line, sizeof(line)) || !catalog::parseStatus(line, code)) {
    setError(tr(S::NoAnswer));
    return false;
  }
  length = 0;
  while (readLine(c, line, sizeof(line))) {
    const size_t n = strlen(line);
    if (n && line[n - 1] == '\r') line[n - 1] = '\0';
    if (!line[0]) break;  // end of the headers
    catalog::parseContentLength(line, length);
  }
  if (code == 200) return true;
  // The server says why on an "error" line after the protocol line.
  char text[96];
  snprintf(text, sizeof(text), "HTTP %d", code);
  if (readLine(c, line, sizeof(line)) && strcmp(line, catalog::kProtocol) == 0 &&
      readLine(c, line, sizeof(line))) {
    char *f[2];
    if (catalog::splitFields(line, f, 2) == 2 && strcmp(f[0], "error") == 0) {
      strlcpy(text, f[1], sizeof(text));
    }
  }
  setError(text);
  return false;
}

// A text reply: every record after the protocol line goes to `onRecord`.
template <typename F>
bool getRecords(const String &target, F onRecord) {
  WiFiClient c;
  uint32_t length = 0;
  if (!get(c, target, length)) {
    c.stop();
    return false;
  }
  char line[kLineBytes];
  uint32_t left = length ? length : 0xFFFFFFFFu;
  if (!readLine(c, line, sizeof(line), &left) || strcmp(line, catalog::kProtocol) != 0) {
    c.stop();
    setError("not a book server (wrong address?)");
    return false;
  }
  while (readLine(c, line, sizeof(line), &left)) {
    char *f[6];
    const int n = catalog::splitFields(line, f, 6);
    onRecord(f, n);
  }
  c.stop();
  return true;
}

String enc(const char *value) {
  char out[160];
  catalog::urlEncode(value, out, sizeof(out));
  return String(out);
}

void copyRow(Row &r, char **f, int n) {
  strlcpy(r.id, n > 1 ? f[1] : "", sizeof(r.id));
  strlcpy(r.title, n > 2 ? f[2] : "", sizeof(r.title));
  strlcpy(r.detail, n > 3 ? f[3] : "", sizeof(r.detail));
  strlcpy(r.file, n > 4 ? f[4] : "", sizeof(r.file));
}

}  // namespace

String address() {
  String a = prefs::bookServer();
#ifdef BOOK_SERVER
  if (!a.length()) a = BOOK_SERVER;
#endif
  return a;
}

Join connect(char *ssid, size_t ssidSize) {
  ssid[0] = '\0';
  const String a = address();
  if (!a.length()) return Join::NoServerSet;
  if (!catalog::parseAddress(a.c_str(), addr)) return Join::BadAddress;
  snprintf(hostText, sizeof(hostText), "%s:%u", addr.host, unsigned(addr.port));
  if (joined && WiFi.status() == WL_CONNECTED) return Join::Ok;
  if (!transfer::joinHomeWifi(ssid, ssidSize)) return ssid[0] ? Join::WifiFailed : Join::NoWifiSet;
  joined = true;
  Serial.printf("book server %s over %s, reader at %s\n", hostText, ssid,
                WiFi.localIP().toString().c_str());
  return Join::Ok;
}

void disconnect() {
  if (!joined) return;
  if (mdnsStarted) MDNS.end();
  mdnsStarted = false;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  joined = false;
  Serial.println("book server: Wi-Fi off");
}

bool connected() { return joined; }
const char *host() { return hostText; }
const char *error() { return lastError; }

bool profiles(const char *screen, std::vector<Row> &out) {
  out.clear();
  return getRecords("/api/profiles?screen=" + enc(screen), [&](char **f, int n) {
    if (n >= 3 && strcmp(f[0], "profile") == 0) {
      out.emplace_back();
      copyRow(out.back(), f, n);
    }
  });
}

bool list(const char *what, const char *query, uint32_t offset, uint32_t count, uint32_t &total,
          std::vector<Row> &out) {
  out.clear();
  total = 0;
  char target[160];
  snprintf(target, sizeof(target), "/api/%s?%s&offset=%u&count=%u", what, query, unsigned(offset),
           unsigned(count));
  return getRecords(target, [&](char **f, int n) {
    if (n >= 2 && strcmp(f[0], "total") == 0) {
      total = uint32_t(strtoul(f[1], nullptr, 10));
    } else if (n >= 3 && (strcmp(f[0], "book") == 0 || strcmp(f[0], "author") == 0)) {
      out.emplace_back();
      copyRow(out.back(), f, n);
    }
  });
}

bool prepare(const char *profile, const char *book, uint32_t room, Prepared &out) {
  out = Prepared();
  out.state = Prepared::State::Failed;
  String target = "/api/prepare?profile=" + enc(profile) + "&book=" + enc(book);
  if (room) target += "&room=" + String(room);
  bool answered = false;
  const bool ok = getRecords(target, [&](char **f, int n) {
    if (strcmp(f[0], "busy") == 0 && n >= 3) {
      out.state = Prepared::State::Busy;
      out.percent = atoi(f[1]);
      out.ahead = atoi(f[2]);
      answered = true;
    } else if (strcmp(f[0], "ready") == 0 && n >= 4) {
      out.state = Prepared::State::Ready;
      out.pages = uint32_t(strtoul(f[1], nullptr, 10));
      out.bytes = uint32_t(strtoul(f[2], nullptr, 10));
      strlcpy(out.file, f[3], sizeof(out.file));
      answered = true;
    } else if (strcmp(f[0], "part") == 0 && n >= 4) {
      Part p;
      p.first = uint32_t(strtoul(f[1], nullptr, 10));
      p.last = uint32_t(strtoul(f[2], nullptr, 10));
      p.bytes = uint32_t(strtoul(f[3], nullptr, 10));
      out.parts.push_back(p);
    } else if (strcmp(f[0], "error") == 0 && n >= 2) {
      out.state = Prepared::State::Failed;
      setError(f[1]);
      answered = true;
    }
  });
  if (ok && !answered) setError("unexpected reply");
  return ok && answered;
}

bool download(const char *profile, const char *book, uint32_t first, uint32_t last,
              const String &path, Progress progress, void *ctx) {
  String target = "/api/download?profile=" + enc(profile) + "&book=" + enc(book);
  if (first && last) target += "&first=" + String(first) + "&last=" + String(last);
  WiFiClient c;
  uint32_t length = 0;
  if (!get(c, target, length)) {
    c.stop();
    return false;
  }
  if (!length) {
    c.stop();
    setError("unexpected reply");
    return false;
  }
  storage::files().remove(path);
  fs::File f = storage::files().open(path, FILE_WRITE);
  if (!f) {
    c.stop();
    setError(tr(S::CannotWrite));
    return false;
  }
  static uint8_t buf[4096];
  uint32_t done = 0;
  uint32_t lastData = millis();
  bool ok = true;
  if (progress) progress(0, length, ctx);
  while (done < length) {
    const int avail = c.available();
    if (avail <= 0) {
      if (!c.connected() || millis() - lastData > kReadMs) {
        setError(tr(S::ConnectionLost));
        ok = false;
        break;
      }
      delay(2);
      continue;
    }
    const size_t want = min<size_t>(min<size_t>(sizeof(buf), size_t(avail)), length - done);
    const int got = c.read(buf, want);
    if (got <= 0) continue;
    lastData = millis();
    if (f.write(buf, size_t(got)) != size_t(got)) {
      setError(tr(S::NotEnoughRoom));
      ok = false;
      break;
    }
    done += uint32_t(got);
    if (progress) progress(done, length, ctx);
  }
  c.stop();
  f.close();
  if (ok) {  // close() flushes the last bytes and can't say whether the card was full
    fs::File check = storage::files().open(path, FILE_READ);
    ok = check && check.size() == length;
    if (check) check.close();
    if (!ok) setError(tr(S::NotEnoughRoom));
  }
  if (!ok) storage::files().remove(path);
  return ok;
}

}  // namespace bookserver
