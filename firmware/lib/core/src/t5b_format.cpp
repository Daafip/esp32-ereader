#include "t5b_format.h"

#include <stdio.h>
#include <string.h>

namespace t5b {
namespace {

uint16_t rd16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }

uint32_t rd32(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

void copyField(char *dst, const uint8_t *src, size_t n) {
  memcpy(dst, src, n);
  dst[n] = '\0';
}

}  // namespace

const char *errorText(Error e) {
  switch (e) {
    case Error::None: return "ok";
    case Error::TooShort: return "file too short";
    case Error::BadMagic: return "not a .t5b file";
    case Error::BadVersion: return "unsupported .t5b version";
    case Error::BadStride: return "corrupt header (stride)";
    case Error::WrongGeometry: return "made for another screen size";
    case Error::NoPages: return "book has no pages";
    case Error::SizeMismatch: return "file size does not match header";
  }
  return "unknown error";
}

Error parseHeader(const uint8_t *d, size_t len, Header &out) {
  if (len < kHeaderSize) return Error::TooShort;
  if (memcmp(d, "T5B1", 4) != 0) return Error::BadMagic;
  out.version = rd16(d + 4);
  out.pageW = rd16(d + 6);
  out.pageH = rd16(d + 8);
  out.stride = rd16(d + 10);
  out.pageCount = rd32(d + 12);
  out.tocCount = rd16(d + 16);
  copyField(out.title, d + 18, kTitleBytes);
  copyField(out.author, d + 82, kAuthorBytes);
  if (out.version != kVersion) return Error::BadVersion;
  if (out.stride != (out.pageW + 7) / 8) return Error::BadStride;
  return Error::None;
}

Error validate(const Header &h, uint16_t pageW, uint16_t pageH, uint32_t fileSize) {
  if (h.pageW != pageW || h.pageH != pageH) return Error::WrongGeometry;
  if (h.pageCount == 0) return Error::NoPages;
  if (fileSize < h.fileSize()) return Error::SizeMismatch;
  return Error::None;
}

Error parseCover(const Header &h, uint32_t fileSize, const uint8_t *head, size_t len, Cover &out) {
  out = Cover();
  const uint32_t end = h.fileSize();
  if (fileSize == end) return Error::None;  // no cover
  if (fileSize < end + kCoverHeaderSize || !head || len < kCoverHeaderSize ||
      memcmp(head, "T5CV", 4) != 0) {
    return Error::SizeMismatch;
  }
  out.w = rd16(head + 4);
  out.h = rd16(head + 6);
  out.stride = rd16(head + 8);
  if (out.w == 0 || out.h == 0 || out.stride != (out.w + 7) / 8 ||
      uint64_t(end) + kCoverHeaderSize + uint64_t(out.stride) * out.h != fileSize) {
    out = Cover();
    return Error::SizeMismatch;
  }
  out.offset = end + kCoverHeaderSize;
  out.present = true;
  return Error::None;
}

void parseTocEntry(const uint8_t *d, TocEntry &out) {
  out.startPage = rd32(d);
  copyField(out.title, d + 4, kTocTitleBytes);
}

int chapterForPage(const uint32_t *starts, int count, uint32_t page) {
  int lo = 0, hi = count;  // first index with starts[i] > page
  while (lo < hi) {
    const int mid = (lo + hi) / 2;
    if (starts[mid] <= page) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo - 1;
}

uint32_t bookId(const char *title, uint32_t pageCount) {
  uint32_t h = 2166136261u;
  auto mix = [&h](uint8_t b) {
    h ^= b;
    h *= 16777619u;
  };
  for (const char *p = title; *p; ++p) mix(uint8_t(*p));
  mix(0);
  for (int i = 0; i < 4; i++) mix(uint8_t(pageCount >> (8 * i)));
  return h;
}

void bookmarkKey(uint32_t id, char out[10]) { snprintf(out, 10, "b%08x", (unsigned)id); }

}  // namespace t5b
