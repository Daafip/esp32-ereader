#include "syncer.h"

#include <ESPmDNS.h>
#include <WiFi.h>

#include <memory>

#include "board.h"
#include "mqtt_packet.h"
#include "prefs.h"
#include "storage.h"
#include "t5b_format.h"
#include "transfer.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

namespace syncer {
namespace {

constexpr uint32_t kConnectMs = 4000;  // TCP
constexpr uint32_t kReplyMs = 4000;    // CONNACK, SUBACK, the PUBACKs
constexpr uint32_t kRecordsMs = 5000;  // the retained records, until our marker is back
constexpr uint16_t kKeepAliveS = 30;
constexpr size_t kStateMaxBytes = 64 << 10;
constexpr uint16_t kSubscribeId = 1;

bool startedWifi = false;  // switched on here: off again afterwards
bool mdnsStarted = false;
char devId[9] = "";
void (*idleHook)() = nullptr;

WiFiClient client;
uint8_t rx[1024];  // one incoming packet: a record is about 200 bytes

void idle() {
  if (idleHook) idleHook();
}

String statePath(const String &stem) { return String(kSyncDir) + "/" + stem + ".txt"; }

// The broker's IP: an address, a name from DNS, or a .local name over mDNS.
bool resolve(const char *host, IPAddress &ip) {
  if (ip.fromString(host)) return true;
  const size_t n = strlen(host);
  if (n > 6 && strcasecmp(host + n - 6, ".local") == 0) {
    // The Wi-Fi transfer runs its own responder; start one only without it.
    if (!mdnsStarted && !transfer::info().running) mdnsStarted = MDNS.begin("t5reader");
    String name(host);
    name.remove(name.length() - 6);
    ip = MDNS.queryHost(name, 3000);
    return ip != IPAddress();
  }
  return WiFi.hostByName(host, ip) == 1;
}

bool send(const uint8_t *data, size_t n) { return n && client.write(data, n) == n; }

// Reads packets until `done()` is true after one of them, or timeoutMs pass without
// that. Every packet goes to `onPacket`, also those after the one that ends the wait in
// the same piece read (a SUBACK and the first records come together: tried against
// Mosquitto 2.0.20).
template <typename OnPacket, typename Done>
bool readUntil(mqtt::Parser &parser, uint32_t timeoutMs, OnPacket onPacket, Done done) {
  const uint32_t start = millis();
  uint8_t chunk[256];
  while (millis() - start < timeoutMs) {
    idle();
    const int avail = client.available();
    if (avail <= 0) {
      if (!client.connected()) return false;
      delay(2);
      continue;
    }
    const int n = client.read(chunk, avail < int(sizeof(chunk)) ? avail : int(sizeof(chunk)));
    bool finished = false;
    for (int i = 0; i < n; i++) {
      if (parser.push(chunk[i])) {
        onPacket(parser);
        finished = finished || done();
      }
      if (parser.bad()) return false;
    }
    if (finished) return true;
  }
  return false;
}

// A book on the reader, and the record the broker has for it.
struct Item {
  String path;
  String stem;
  String title;
  uint32_t id = 0;
  uint32_t pages = 0;
  char key[9] = "";
  bool hasRemote = false;
  possync::Record remote;
};

// Contents start pages of the book file at `path`.
void readContents(const String &path, std::vector<uint32_t> &out) {
  out.clear();
  fs::File f = storage::files().open(path, FILE_READ);
  uint8_t head[t5b::kHeaderSize];
  t5b::Header h;
  if (!f || f.read(head, sizeof(head)) != sizeof(head) ||
      t5b::parseHeader(head, sizeof(head), h) != t5b::Error::None) {
    if (f) f.close();
    return;
  }
  const int count = h.tocCount < 2048 ? h.tocCount : 2048;  // as Book::kMaxToc
  uint8_t entries[32 * t5b::kTocEntrySize];
  for (int i = 0; i < count;) {
    const int n = count - i < 32 ? count - i : 32;
    const size_t bytes = size_t(n) * t5b::kTocEntrySize;
    if (f.read(entries, bytes) != bytes) break;
    for (int j = 0; j < n; j++) {
      const uint8_t *e = entries + j * t5b::kTocEntrySize;
      out.push_back(uint32_t(e[0]) | uint32_t(e[1]) << 8 | uint32_t(e[2]) << 16 |
                    uint32_t(e[3]) << 24);
    }
    i += n;
  }
  f.close();
}

// "Dieren eten (152–297)" -> "Dieren eten": the whole book's title in a record.
String wholeTitle(const String &title, bool part) {
  if (!part) return title;
  const int open = title.lastIndexOf(" (");
  return open > 0 ? title.substring(0, open) : title;
}

const char *readerName() {
  static char name[33];
  const String n = prefs::deviceName();
  strlcpy(name, n.length() ? n.c_str() : BOARD_NAME, sizeof(name));
  return name;
}

void fail(Report &r, Result result, const char *why) {
  r.result = result;
  Serial.printf("sync: %s (%s)\n", why, r.detail);
}

// The MQTT part of a session, once the Wi-Fi is up.
void exchange(Report &r, const char *host, uint16_t port) {
  std::vector<storage::BookEntry> entries;
  storage::listAllBooks(entries);
  std::vector<Item> books;
  books.reserve(entries.size());
  for (const storage::BookEntry &e : entries) {
    Item it;
    it.path = e.path;
    it.stem = storage::stemOf(e.path);
    it.title = e.title;
    it.id = e.id;
    it.pages = e.pages;
    possync::bookKey(it.stem.c_str(), it.key);
    books.push_back(it);
  }

  IPAddress ip;
  if (!resolve(host, ip)) return fail(r, Result::Unreachable, "broker name not found");
  if (!client.connect(ip, port, kConnectMs)) return fail(r, Result::Unreachable, "no connection");
  client.setNoDelay(true);
  std::unique_ptr<uint8_t[]> out(new uint8_t[768]);
  const size_t outSize = 768;
  mqtt::Parser parser(rx, sizeof(rx));

  int connack = -1;
  bool subscribed = false, marker = false;
  std::vector<uint16_t> acked;
  char markerTopic[32];
  snprintf(markerTopic, sizeof(markerTopic), "t5reader/sync/%s", devId);
  const size_t prefixLen = strlen(possync::kTopicPrefix);
  auto onPacket = [&](mqtt::Parser &p) {
    uint16_t id = 0;
    switch (p.type()) {
      case mqtt::kConnack: connack = mqtt::connackCode(p.body(), p.length()); break;
      case mqtt::kSuback:
        if (mqtt::ackId(p.body(), p.length(), id) && id == kSubscribeId) subscribed = true;
        break;
      case mqtt::kPuback:
        if (mqtt::ackId(p.body(), p.length(), id)) acked.push_back(id);
        break;
      case mqtt::kPublish: {
        mqtt::Message m;
        if (!mqtt::parsePublish(p.flags(), p.body(), p.length(), m)) break;
        if (m.qos == 1) {
          uint8_t ack[4];
          send(ack, mqtt::buildPuback(ack, sizeof(ack), m.packetId));
        }
        if (m.topicLen == strlen(markerTopic) && memcmp(m.topic, markerTopic, m.topicLen) == 0) {
          marker = true;
          break;
        }
        // A record: only the retained ones before the marker (ours come back after it).
        if (marker || m.topicLen != prefixLen + 8 ||
            memcmp(m.topic, possync::kTopicPrefix, prefixLen) != 0) {
          break;
        }
        for (Item &it : books) {
          if (memcmp(m.topic + prefixLen, it.key, 8) != 0) continue;
          possync::Record rec;
          if (possync::parseRecord(reinterpret_cast<const char *>(m.payload), m.payloadLen, rec)) {
            it.remote = rec;
            it.hasRemote = true;
          }
        }
        break;
      }
      default: break;
    }
  };

  char clientId[24];
  snprintf(clientId, sizeof(clientId), "t5reader-%s", devId);
  String user = prefs::mqttUser(), pass = prefs::mqttPassword();
#ifdef MQTT_BROKER
  if (!prefs::mqttBroker().length()) {  // the broker from secrets.h: its login too
#ifdef MQTT_USER
    user = MQTT_USER;
#endif
#ifdef MQTT_PASSWORD
    pass = MQTT_PASSWORD;
#endif
  }
#endif
  if (!send(out.get(), mqtt::buildConnect(out.get(), outSize, clientId, user.c_str(),
                                           pass.c_str(), kKeepAliveS)) ||
      !readUntil(parser, kReplyMs, onPacket, [&] { return connack >= 0; })) {
    client.stop();
    return fail(r, Result::Unreachable, "no answer to CONNECT");
  }
  if (connack != 0) {  // Mosquitto: 5 for a wrong password and for none
    client.stop();
    Serial.printf("sync: CONNACK %d\n", connack);
    return fail(r, connack == 4 || connack == 5 ? Result::Refused : Result::Failed,
                "connection refused");
  }
  const char *filters[] = {"t5reader/progress/+", markerTopic};
  static const uint8_t kEnd[] = {'e', 'n', 'd'};
  if (!send(out.get(), mqtt::buildSubscribe(out.get(), outSize, kSubscribeId, filters, 2)) ||
      !readUntil(parser, kReplyMs, onPacket, [&] { return subscribed; }) ||
      !send(out.get(),
            mqtt::buildPublish(out.get(), outSize, markerTopic, kEnd, sizeof(kEnd), 0, false, 0))) {
    client.stop();
    return fail(r, Result::Failed, "no answer to SUBSCRIBE");
  }
  // Mosquitto sends a client's messages in order: once the marker is back, every
  // retained record has come. Without it, carry on with what came.
  if (!readUntil(parser, kRecordsMs, onPacket, [&] { return marker; })) {
    Serial.println("sync: the marker didn't come back; going on with the records so far");
    marker = true;
  }

  struct Sent {
    size_t book;
    uint16_t id;
    uint32_t rev, local, pages;
  };
  std::vector<Sent> sent;
  uint16_t nextId = kSubscribeId + 1;
  for (size_t i = 0; i < books.size(); i++) {
    Item &it = books[i];
    possync::BookState st;
    load(it.stem, st);
    if (!syncable(st, it.title.c_str(), it.pages)) continue;
    const uint32_t mark = prefs::bookmark(it.id);
    if (!it.hasRemote && !st.synced && !st.dirty && mark == 0) continue;  // never read
    std::vector<uint32_t> contents;
    if (!st.part) readContents(it.path, contents);
    const possync::Layout l = layout(st, it.pages, contents);
    const uint32_t local = l.first + (mark < it.pages ? mark : it.pages - 1);
    const possync::Record *remote = it.hasRemote ? &it.remote : nullptr;
    const uint32_t mapped = remote ? possync::wholePageFor(l, remote->pos) : 0;
    switch (possync::decide(st, local, l.wholePages, remote, mapped, devId)) {
      case possync::Action::Nothing: break;
      case possync::Action::Seen:
        possync::markSynced(st, remote->rev, local, l.wholePages);
        save(it.stem, st);
        break;
      case possync::Action::Ask:
        possync::markAsk(st, *remote);
        save(it.stem, st);
        r.received++;
        Serial.printf("sync: %s: %s went to whole page %u (here %u)\n", it.stem.c_str(),
                      remote->name, unsigned(mapped + 1), unsigned(local + 1));
        break;
      case possync::Action::Publish: {
        possync::Record rec;
        rec.rev = possync::newRev(st, remote);
        rec.pos = possync::positionOf(l, local);
        strlcpy(rec.device, devId, sizeof(rec.device));
        strlcpy(rec.name, readerName(), sizeof(rec.name));
        strlcpy(rec.title, wholeTitle(it.title, st.part).c_str(), sizeof(rec.title));
        const std::string payload = possync::formatRecord(rec);
        const String topic = String(possync::kTopicPrefix) + it.key;
        const uint16_t id = nextId++;
        if (!send(out.get(), mqtt::buildPublish(out.get(), outSize, topic.c_str(),
                                                 reinterpret_cast<const uint8_t *>(payload.data()),
                                                 payload.size(), 1, true, id))) {
          break;
        }
        sent.push_back({i, id, rec.rev, local, l.wholePages});
        break;
      }
    }
  }
  // The broker has stored a record once it answers with PUBACK.
  readUntil(parser, kReplyMs, onPacket, [&] { return acked.size() >= sent.size(); });
  for (const Sent &s : sent) {
    bool ok = false;
    for (uint16_t id : acked) ok = ok || id == s.id;
    if (!ok) continue;
    possync::BookState st;
    load(books[s.book].stem, st);
    possync::markSynced(st, s.rev, s.local, s.pages);
    save(books[s.book].stem, st);
    r.sent++;
  }
  send(out.get(), mqtt::buildDisconnect(out.get(), outSize));
  client.flush();
  client.stop();
  if (r.sent < sent.size()) return fail(r, Result::Failed, "not every record was stored");
  Serial.printf("sync: %u books, %u sent, %u to ask about\n", unsigned(books.size()),
                unsigned(r.sent), unsigned(r.received));
}

}  // namespace

String broker() {
  String b = prefs::mqttBroker();
#ifdef MQTT_BROKER
  if (!b.length()) b = MQTT_BROKER;
#endif
  return b;
}

bool configured() { return broker().length() > 0; }

const char *deviceId() {
  if (!devId[0]) {
    const uint64_t mac = ESP.getEfuseMac();
    snprintf(devId, sizeof(devId), "%08x", unsigned(uint32_t(mac >> 32) ^ uint32_t(mac)));
  }
  return devId;
}

void beginJoin() {
  if (startedWifi || WiFi.status() == WL_CONNECTED) return;
  char ssid[33];
  startedWifi = transfer::beginHomeWifi(ssid, sizeof(ssid));
}

void endJoin() {
  if (!startedWifi) return;
  if (mdnsStarted) MDNS.end();
  mdnsStarted = false;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  startedWifi = false;
}

// run() without its last step: Wi-Fi off again if this switched it on.
static Report session(uint32_t joinMs) {
  Report r;
  const String b = broker();
  char host[64];
  uint16_t port = 0;
  if (!b.length()) {
    r.result = Result::NoBroker;
    return r;
  }
  strlcpy(r.detail, b.c_str(), sizeof(r.detail));
  if (!possync::parseBroker(b.c_str(), host, sizeof(host), port)) {
    r.result = Result::BadAddress;
    return r;
  }
  snprintf(r.detail, sizeof(r.detail), "%s:%u", host, unsigned(port));
  if (!storage::mounted()) {
    fail(r, Result::Failed, "no card");
    return r;
  }
  const uint32_t start = millis();
  if (WiFi.status() != WL_CONNECTED) {
    char ssid[33] = "";
    if (!startedWifi) {
      if (!transfer::beginHomeWifi(ssid, sizeof(ssid))) {
        r.result = Result::NoWifi;
        return r;
      }
      startedWifi = true;
    } else {
      strlcpy(ssid, WiFi.SSID().c_str(), sizeof(ssid));
    }
    if (!transfer::waitHomeWifi(joinMs, idle)) {
      startedWifi = false;  // waitHomeWifi switched it off
      mdnsStarted = false;
      strlcpy(r.detail, ssid[0] ? ssid : prefs::homeSsid().c_str(), sizeof(r.detail));
      fail(r, Result::WifiFailed, "home Wi-Fi not joined");
      return r;
    }
  }
  Serial.printf("sync: Wi-Fi after %u ms, broker %s\n", unsigned(millis() - start), r.detail);
  exchange(r, host, port);
  Serial.printf("sync: done after %u ms\n", unsigned(millis() - start));
  return r;
}

Report run(uint32_t joinMs, void (*idleFn)()) {
  idleHook = idleFn;
  deviceId();
  const Report r = session(joinMs);
  endJoin();
  idleHook = nullptr;
  return r;
}

void load(const String &stem, possync::BookState &out) {
  out = possync::BookState();
  fs::File f = storage::files().open(statePath(stem), FILE_READ);
  if (!f) return;
  const size_t size = f.size() < kStateMaxBytes ? f.size() : kStateMaxBytes;
  std::unique_ptr<char[]> buf(new char[size + 1]);
  const size_t n = f.read(reinterpret_cast<uint8_t *>(buf.get()), size);
  f.close();
  possync::parseState(buf.get(), n, out);
}

bool save(const String &stem, const possync::BookState &s) {
  const std::string text = possync::formatState(s);
  const String path = statePath(stem);
  if (text.empty()) {
    storage::files().remove(path);
    return true;
  }
  storage::files().mkdir(storage::kBooksDir);
  storage::files().mkdir(kSyncDir);
  fs::File f = storage::files().open(path, FILE_WRITE);
  if (!f) return false;
  const bool ok = f.write(reinterpret_cast<const uint8_t *>(text.data()), text.size()) ==
                  text.size();
  f.close();
  return ok;
}

void forget(const String &stem) { storage::files().remove(statePath(stem)); }

possync::Layout layout(const possync::BookState &s, uint32_t pages,
                       const std::vector<uint32_t> &contents) {
  possync::Layout l;
  l.pages = pages;
  if (s.part) {
    l.first = s.first;
    l.wholePages = s.wholePages;
    l.starts = s.starts.data();
    l.tocCount = int(s.starts.size());
  } else {
    l.wholePages = pages;
    l.starts = contents.data();
    l.tocCount = int(contents.size());
  }
  return l;
}

bool syncable(const possync::BookState &s, const char *title, uint32_t pages) {
  if (!pages) return false;
  if (s.part) return s.first + pages <= s.wholePages;
  uint32_t first = 0, last = 0;
  return !(possync::partRange(title, first, last) && last - first + 1 == pages);
}

void installedWhole(const String &stem) {
  possync::BookState s;
  load(stem, s);
  if (!s.part && !s.elsewhere) return;  // nothing changes
  possync::setWhole(s);
  save(stem, s);
}

void installedPart(const String &stem, uint32_t first, uint32_t wholePages,
                   const std::vector<uint32_t> &starts) {
  possync::BookState s;
  load(stem, s);
  possync::setPart(s, first, wholePages, starts);
  save(stem, s);
}

}  // namespace syncer
