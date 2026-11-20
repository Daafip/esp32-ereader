#include "mqtt_packet.h"

#include <string.h>

namespace mqtt {
namespace {

// Writes into a buffer and remembers whether everything fitted.
struct Writer {
  uint8_t *out;
  size_t size;
  size_t n = 0;
  bool ok = true;

  void byte(uint8_t b) {
    if (n < size) {
      out[n++] = b;
    } else {
      ok = false;
    }
  }
  void u16(uint16_t v) {
    byte(uint8_t(v >> 8));
    byte(uint8_t(v));
  }
  void bytes(const void *data, size_t len) {
    if (len > size - n || n > size) {
      ok = false;
      return;
    }
    memcpy(out + n, data, len);
    n += len;
  }
  void str(const char *s) {  // UTF-8 string: two length bytes, then the text
    const size_t len = strlen(s);
    if (len > 0xFFFF) {
      ok = false;
      return;
    }
    u16(uint16_t(len));
    bytes(s, len);
  }
};

size_t strSize(const char *s) { return 2 + strlen(s); }

// The fixed header for a packet of `remaining` bytes after it.
bool header(Writer &w, uint8_t first, size_t remaining) {
  uint8_t len[4];
  const size_t n = remaining <= 0x0FFFFFFF ? encodeLength(uint32_t(remaining), len) : 0;
  if (!n) return false;
  w.byte(first);
  w.bytes(len, n);
  return w.ok;
}

size_t done(const Writer &w) { return w.ok ? w.n : 0; }

}  // namespace

size_t encodeLength(uint32_t n, uint8_t out[4]) {
  if (n > 0x0FFFFFFF) return 0;
  size_t i = 0;
  do {
    uint8_t b = uint8_t(n % 128);
    n /= 128;
    if (n) b |= 0x80;
    out[i++] = b;
  } while (n);
  return i;
}

size_t buildConnect(uint8_t *out, size_t size, const char *clientId, const char *user,
                    const char *password, uint16_t keepAliveS) {
  const bool hasUser = user && *user;
  const bool hasPassword = hasUser && password && *password;  // 3.1.1: no password alone
  const size_t remaining = 10 + strSize(clientId) + (hasUser ? strSize(user) : 0) +
                           (hasPassword ? strSize(password) : 0);
  Writer w{out, size};
  if (!header(w, uint8_t(kConnect << 4), remaining)) return 0;
  w.str("MQTT");
  w.byte(4);  // protocol level 3.1.1
  w.byte(uint8_t(0x02 | (hasUser ? 0x80 : 0) | (hasPassword ? 0x40 : 0)));  // clean session
  w.u16(keepAliveS);
  w.str(clientId);
  if (hasUser) w.str(user);
  if (hasPassword) w.str(password);
  return done(w);
}

size_t buildSubscribe(uint8_t *out, size_t size, uint16_t packetId, const char *const *filters,
                      int count) {
  if (count <= 0) return 0;
  size_t remaining = 2;
  for (int i = 0; i < count; i++) remaining += strSize(filters[i]) + 1;
  Writer w{out, size};
  if (!header(w, uint8_t(kSubscribe << 4 | 0x02), remaining)) return 0;  // flags 0010
  w.u16(packetId);
  for (int i = 0; i < count; i++) {
    w.str(filters[i]);
    w.byte(0);  // QoS 0
  }
  return done(w);
}

size_t buildPublish(uint8_t *out, size_t size, const char *topic, const uint8_t *payload,
                    size_t len, uint8_t qos, bool retain, uint16_t packetId) {
  if (qos > 1) return 0;
  const size_t remaining = strSize(topic) + (qos ? 2 : 0) + len;
  Writer w{out, size};
  if (!header(w, uint8_t(kPublish << 4 | qos << 1 | (retain ? 1 : 0)), remaining)) return 0;
  w.str(topic);
  if (qos) w.u16(packetId);
  w.bytes(payload, len);
  return done(w);
}

size_t buildPuback(uint8_t *out, size_t size, uint16_t packetId) {
  Writer w{out, size};
  w.byte(uint8_t(kPuback << 4));
  w.byte(2);
  w.u16(packetId);
  return done(w);
}

size_t buildDisconnect(uint8_t *out, size_t size) {
  Writer w{out, size};
  w.byte(uint8_t(kDisconnect << 4));
  w.byte(0);
  return done(w);
}

bool Parser::push(uint8_t b) {
  if (bad_) return false;
  switch (state_) {
    case State::First:
      first_ = b;
      length_ = 0;
      multiplier_ = 1;
      lengthBytes_ = 0;
      state_ = State::Length;
      return false;
    case State::Length:
      length_ += uint32_t(b & 0x7F) * multiplier_;
      multiplier_ *= 128;
      if (++lengthBytes_ > 4 || (lengthBytes_ == 4 && (b & 0x80))) {
        bad_ = true;
        return false;
      }
      if (b & 0x80) return false;
      got_ = 0;
      if (length_ == 0) {
        state_ = State::First;
        return true;
      }
      state_ = State::Body;
      return false;
    case State::Body:
      if (got_ < size_) buf_[got_] = b;
      if (++got_ < length_) return false;
      state_ = State::First;
      if (length_ > size_) {  // didn't fit: skipped
        dropped_++;
        return false;
      }
      return true;
  }
  return false;
}

bool parsePublish(uint8_t flags, const uint8_t *body, size_t len, Message &out) {
  out = Message();
  out.qos = uint8_t((flags >> 1) & 3);
  out.retain = flags & 1;
  if (out.qos > 2 || len < 2) return false;
  const size_t topicLen = size_t(body[0]) << 8 | body[1];
  size_t at = 2 + topicLen;
  if (at > len) return false;
  out.topic = reinterpret_cast<const char *>(body + 2);
  out.topicLen = topicLen;
  if (out.qos) {
    if (at + 2 > len) return false;
    out.packetId = uint16_t(body[at] << 8 | body[at + 1]);
    at += 2;
  }
  out.payload = body + at;
  out.payloadLen = len - at;
  return true;
}

int connackCode(const uint8_t *body, size_t len) { return len == 2 ? body[1] : -1; }

bool ackId(const uint8_t *body, size_t len, uint16_t &id, uint8_t *granted) {
  if (len < 2) return false;
  id = uint16_t(body[0] << 8 | body[1]);
  if (granted) *granted = len > 2 ? body[2] : 0x80;
  return true;
}

}  // namespace mqtt
