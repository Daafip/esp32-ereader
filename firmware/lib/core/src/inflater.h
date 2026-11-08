// Inflates zlib streams (RFC 1950 around RFC 1951 deflate) into a flat buffer: the
// compressed pages of a version 2 .t5b. Small on purpose: the output buffer holds the
// whole page, so back-references point into it and no 32 KB window is needed, and the
// Huffman codes are decoded bit by bit from count/symbol tables, after Mark Adler's
// puff.c. About 1.8 KB of state; a 48 KB page takes a few milliseconds.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace zinf {

// Where the compressed bytes come from: fills buf with up to max bytes, 0 at the end.
class Source {
 public:
  virtual size_t pull(uint8_t *buf, size_t max) = 0;

 protected:
  ~Source() = default;
};

enum class Status : uint8_t {
  Ok,
  Truncated,    // the input ended before the stream did
  BadHeader,    // not a zlib stream, or one that needs a preset dictionary
  BadData,      // invalid deflate data
  TooLong,      // inflates to more than the output buffer
  TooShort,     // inflates to less than the output buffer
  BadChecksum,  // Adler-32 of the output doesn't match the stream's
};

const char *statusText(Status s);

uint32_t adler32(const uint8_t *data, size_t len);

class Inflater {
 public:
  // Inflates one zlib stream from src into out, which it must fill exactly. Bytes
  // after the stream's checksum are left unread.
  Status zlib(Source &src, uint8_t *out, size_t outLen);
  // After zlib(): bytes it pulled from the source but didn't use.
  size_t unused() const { return inLen_ - inPos_; }

 private:
  static constexpr int kMaxBits = 15;
  static constexpr int kMaxLitLen = 288;  // 286 used, 2 more in the fixed code
  static constexpr int kMaxDist = 30;

  void fail(Status s);
  bool refill();
  uint32_t bits(int need);
  int decode(const uint16_t *count, const uint16_t *symbol);
  static int construct(uint16_t *count, uint16_t *symbol, const uint8_t *length, int n);
  void stored();
  void fixed();
  void dynamic();
  void codes();

  Source *src_ = nullptr;
  uint8_t *out_ = nullptr;
  size_t outLen_ = 0, outPos_ = 0;
  Status status_ = Status::Ok;
  uint32_t bitBuf_ = 0;
  int bitCnt_ = 0;
  size_t inPos_ = 0, inLen_ = 0;
  uint16_t lenCount_[kMaxBits + 1], lenSymbol_[kMaxLitLen];
  uint16_t distCount_[kMaxBits + 1], distSymbol_[kMaxDist];
  uint8_t in_[1024];
};

}  // namespace zinf
