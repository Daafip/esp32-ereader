// MQTT 3.1.1 packets for one short session (position sync, src/syncer.cpp): CONNECT,
// SUBSCRIBE, PUBLISH, PUBACK and DISCONNECT out; CONNACK, SUBACK, PUBLISH and PUBACK
// in. No Arduino, tested on the PC against byte sequences from the MQTT 3.1.1 text.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace mqtt {

enum Type : uint8_t {
  kConnect = 1,
  kConnack = 2,
  kPublish = 3,
  kPuback = 4,
  kSubscribe = 8,
  kSuback = 9,
  kPingresp = 13,
  kDisconnect = 14,
};

// The remaining length (1 to 4 bytes); 0 if n is too big for MQTT (256 MB).
size_t encodeLength(uint32_t n, uint8_t out[4]);

// Each writes a whole packet into `out` and returns its length, or 0 if it doesn't fit.
// CONNECT with a clean session; user and password left out when empty.
size_t buildConnect(uint8_t *out, size_t size, const char *clientId, const char *user,
                    const char *password, uint16_t keepAliveS);
// SUBSCRIBE to `count` topic filters at QoS 0.
size_t buildSubscribe(uint8_t *out, size_t size, uint16_t packetId, const char *const *filters,
                      int count);
// PUBLISH at QoS 0 (packetId unused) or 1.
size_t buildPublish(uint8_t *out, size_t size, const char *topic, const uint8_t *payload,
                    size_t len, uint8_t qos, bool retain, uint16_t packetId);
size_t buildPuback(uint8_t *out, size_t size, uint16_t packetId);
size_t buildDisconnect(uint8_t *out, size_t size);

// Incoming packets, fed a byte at a time (TCP hands them over in any pieces).
class Parser {
 public:
  Parser(uint8_t *buf, size_t size) : buf_(buf), size_(size) {}
  // True when a whole packet is in: type(), flags(), body() and length() describe it
  // until the next push(). A packet longer than the buffer is skipped (dropped() counts
  // them); a malformed length makes bad() true and the rest is ignored.
  bool push(uint8_t b);
  uint8_t type() const { return uint8_t(first_ >> 4); }
  uint8_t flags() const { return uint8_t(first_ & 0x0F); }
  const uint8_t *body() const { return buf_; }
  size_t length() const { return length_; }
  bool bad() const { return bad_; }
  uint32_t dropped() const { return dropped_; }

 private:
  enum class State : uint8_t { First, Length, Body };
  uint8_t *buf_;
  size_t size_;
  State state_ = State::First;
  uint8_t first_ = 0;
  uint32_t length_ = 0;  // remaining length of the packet coming in
  uint32_t got_ = 0;
  uint32_t multiplier_ = 1;
  int lengthBytes_ = 0;
  bool bad_ = false;
  uint32_t dropped_ = 0;
};

struct Message {
  const char *topic = nullptr;  // not terminated: topicLen bytes
  size_t topicLen = 0;
  const uint8_t *payload = nullptr;
  size_t payloadLen = 0;
  uint8_t qos = 0;
  bool retain = false;
  uint16_t packetId = 0;  // QoS 1 and 2 only
};
// A PUBLISH packet's body (flags from its first byte).
bool parsePublish(uint8_t flags, const uint8_t *body, size_t len, Message &out);
// CONNACK's return code: 0 accepted, 1-3 refused (protocol, client id, server), 4 bad
// user name or password, 5 not authorized; -1 if malformed.
int connackCode(const uint8_t *body, size_t len);
// The packet id of a PUBACK or SUBACK; false if malformed. SUBACK: `granted` gets the
// first return code (0x80: refused).
bool ackId(const uint8_t *body, size_t len, uint16_t &id, uint8_t *granted = nullptr);

}  // namespace mqtt
