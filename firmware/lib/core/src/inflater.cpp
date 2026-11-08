#include "inflater.h"

#include <string.h>

namespace zinf {
namespace {

// RFC 1951 3.2.5: base values and extra bits of the length and distance symbols
const uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                               31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                               2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
// RFC 1951 3.2.7: order of the code length code lengths
const uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

}  // namespace

const char *statusText(Status s) {
  switch (s) {
    case Status::Ok: return "ok";
    case Status::Truncated: return "compressed data ends early";
    case Status::BadHeader: return "not a zlib stream";
    case Status::BadData: return "invalid compressed data";
    case Status::TooLong: return "inflates to too many bytes";
    case Status::TooShort: return "inflates to too few bytes";
    case Status::BadChecksum: return "checksum mismatch";
  }
  return "unknown error";
}

uint32_t adler32(const uint8_t *p, size_t n) {
  uint32_t a = 1, b = 0;
  while (n) {
    size_t k = n < 5552 ? n : 5552;  // the most bytes before b can overflow
    n -= k;
    while (k--) {
      a += *p++;
      b += a;
    }
    a %= 65521;
    b %= 65521;
  }
  return (b << 16) | a;
}

void Inflater::fail(Status s) {
  if (status_ == Status::Ok) status_ = s;
}

bool Inflater::refill() {
  if (status_ != Status::Ok) return false;
  inLen_ = src_->pull(in_, sizeof(in_));
  inPos_ = 0;
  if (inLen_ == 0) {
    fail(Status::Truncated);
    return false;
  }
  return true;
}

// `need` bits (0..16), least significant first. After a failed refill: 0, status set.
uint32_t Inflater::bits(int need) {
  uint32_t val = bitBuf_;
  while (bitCnt_ < need) {
    if (inPos_ == inLen_ && !refill()) return 0;
    val |= uint32_t(in_[inPos_++]) << bitCnt_;
    bitCnt_ += 8;
  }
  bitBuf_ = val >> need;
  bitCnt_ -= need;
  return val & ((1u << need) - 1);
}

// One symbol of a canonical Huffman code, a bit at a time (codes are stored MSB first).
int Inflater::decode(const uint16_t *count, const uint16_t *symbol) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len <= kMaxBits; len++) {
    if (bitCnt_ == 0) {
      if (inPos_ == inLen_ && !refill()) return -1;
      bitBuf_ = in_[inPos_++];
      bitCnt_ = 8;
    }
    code |= int(bitBuf_ & 1);
    bitBuf_ >>= 1;
    bitCnt_--;
    const int n = count[len];
    if (code - n < first) return symbol[index + (code - first)];
    index += n;
    first += n;
    first <<= 1;
    code <<= 1;
  }
  fail(Status::BadData);  // a code the table doesn't have
  return -1;
}

// Builds count/symbol tables from code lengths. 0: complete, < 0: over-subscribed,
// > 0: incomplete (only allowed for a single code).
int Inflater::construct(uint16_t *count, uint16_t *symbol, const uint8_t *length, int n) {
  for (int len = 0; len <= kMaxBits; len++) count[len] = 0;
  for (int s = 0; s < n; s++) count[length[s]]++;
  if (count[0] == n) return 0;  // no codes: decode() fails on any input
  int left = 1;
  for (int len = 1; len <= kMaxBits; len++) {
    left <<= 1;
    left -= count[len];
    if (left < 0) return left;
  }
  uint16_t offs[kMaxBits + 1];
  offs[1] = 0;
  for (int len = 1; len < kMaxBits; len++) offs[len + 1] = uint16_t(offs[len] + count[len]);
  for (int s = 0; s < n; s++) {
    if (length[s]) symbol[offs[length[s]]++] = uint16_t(s);
  }
  return left;
}

void Inflater::stored() {
  bitBuf_ = 0;  // skip to the byte boundary (fewer than 8 bits are ever left over)
  bitCnt_ = 0;
  const uint32_t len = bits(16);
  const uint32_t nlen = bits(16);
  if (status_ != Status::Ok) return;
  if (len != (~nlen & 0xFFFF)) return fail(Status::BadData);
  if (len > outLen_ - outPos_) return fail(Status::TooLong);
  for (uint32_t left = len; left;) {
    if (inPos_ == inLen_ && !refill()) return;
    size_t n = inLen_ - inPos_;
    if (n > left) n = left;
    memcpy(out_ + outPos_, in_ + inPos_, n);
    inPos_ += n;
    outPos_ += n;
    left -= uint32_t(n);
  }
}

void Inflater::codes() {
  for (;;) {
    int sym = decode(lenCount_, lenSymbol_);
    if (sym < 0) return;
    if (sym < 256) {
      if (outPos_ == outLen_) return fail(Status::TooLong);
      out_[outPos_++] = uint8_t(sym);
    } else if (sym == 256) {
      return;  // end of block
    } else {
      sym -= 257;
      if (sym >= 29) return fail(Status::BadData);
      const uint32_t len = kLenBase[sym] + bits(kLenExtra[sym]);
      const int d = decode(distCount_, distSymbol_);
      if (d < 0) return;
      const uint32_t dist = kDistBase[d] + bits(kDistExtra[d]);
      if (status_ != Status::Ok) return;
      if (dist > outPos_) return fail(Status::BadData);  // before the start of the page
      if (len > outLen_ - outPos_) return fail(Status::TooLong);
      uint8_t *to = out_ + outPos_;
      const uint8_t *from = to - dist;
      for (uint32_t i = 0; i < len; i++) to[i] = from[i];  // may overlap: byte by byte
      outPos_ += len;
    }
  }
}

void Inflater::fixed() {
  uint8_t lengths[kMaxLitLen];
  int s = 0;
  for (; s < 144; s++) lengths[s] = 8;
  for (; s < 256; s++) lengths[s] = 9;
  for (; s < 280; s++) lengths[s] = 7;
  for (; s < kMaxLitLen; s++) lengths[s] = 8;
  construct(lenCount_, lenSymbol_, lengths, kMaxLitLen);
  for (s = 0; s < kMaxDist; s++) lengths[s] = 5;
  construct(distCount_, distSymbol_, lengths, kMaxDist);
  codes();
}

void Inflater::dynamic() {
  uint8_t lengths[286 + kMaxDist];
  const int nlen = int(bits(5)) + 257;
  const int ndist = int(bits(5)) + 1;
  const int ncode = int(bits(4)) + 4;
  if (status_ != Status::Ok) return;
  if (nlen > 286 || ndist > kMaxDist) return fail(Status::BadData);

  int index = 0;
  for (; index < ncode; index++) lengths[kOrder[index]] = uint8_t(bits(3));
  for (; index < 19; index++) lengths[kOrder[index]] = 0;
  if (status_ != Status::Ok) return;
  // The code length code, in the literal/length tables for now; it must be complete.
  if (construct(lenCount_, lenSymbol_, lengths, 19) != 0) return fail(Status::BadData);

  index = 0;
  while (index < nlen + ndist) {
    const int sym = decode(lenCount_, lenSymbol_);
    if (sym < 0) return;
    if (sym < 16) {
      lengths[index++] = uint8_t(sym);
      continue;
    }
    uint8_t len = 0;
    int repeat;
    if (sym == 16) {  // repeat the previous length
      if (index == 0) return fail(Status::BadData);
      len = lengths[index - 1];
      repeat = 3 + int(bits(2));
    } else if (sym == 17) {
      repeat = 3 + int(bits(3));
    } else {
      repeat = 11 + int(bits(7));
    }
    if (status_ != Status::Ok) return;
    if (index + repeat > nlen + ndist) return fail(Status::BadData);
    while (repeat--) lengths[index++] = len;
  }
  if (lengths[256] == 0) return fail(Status::BadData);  // no end-of-block code

  int err = construct(lenCount_, lenSymbol_, lengths, nlen);
  if (err < 0 || (err > 0 && nlen != lenCount_[0] + lenCount_[1])) return fail(Status::BadData);
  err = construct(distCount_, distSymbol_, lengths + nlen, ndist);
  if (err < 0 || (err > 0 && ndist != distCount_[0] + distCount_[1])) {
    return fail(Status::BadData);
  }
  codes();
}

Status Inflater::zlib(Source &src, uint8_t *out, size_t outLen) {
  src_ = &src;
  out_ = out;
  outLen_ = outLen;
  outPos_ = 0;
  status_ = Status::Ok;
  bitBuf_ = 0;
  bitCnt_ = 0;
  inPos_ = inLen_ = 0;

  const uint32_t cmf = bits(8);
  const uint32_t flg = bits(8);
  if (status_ != Status::Ok) return status_;
  if ((cmf & 0x0F) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20)) {
    return Status::BadHeader;
  }
  bool last;
  do {
    last = bits(1) != 0;
    const uint32_t type = bits(2);
    if (status_ != Status::Ok) break;
    if (type == 0) {
      stored();
    } else if (type == 1) {
      fixed();
    } else if (type == 2) {
      dynamic();
    } else {
      fail(Status::BadData);
    }
  } while (!last && status_ == Status::Ok);
  if (status_ != Status::Ok) return status_;
  if (outPos_ != outLen_) return Status::TooShort;

  bitBuf_ = 0;  // the Adler-32 starts on a byte boundary, most significant byte first
  bitCnt_ = 0;
  uint32_t check = 0;
  for (int i = 0; i < 4; i++) check = (check << 8) | bits(8);
  if (status_ != Status::Ok) return status_;
  return check == adler32(out_, outLen_) ? Status::Ok : Status::BadChecksum;
}

}  // namespace zinf
