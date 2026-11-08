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
    case Error::BadIndex: return "corrupt page index";
    case Error::BadPage: return "corrupt page";
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
  if (out.version < kOldestVersion || out.version > kVersion) return Error::BadVersion;
  if (out.stride != (out.pageW + 7) / 8) return Error::BadStride;
  // Offsets are u32: a page count that pushes them past 4 GB can't be a real file.
  const uint64_t indexEnd =
      uint64_t(out.indexOffset()) + (out.compressed() ? 4 * (uint64_t(out.pageCount) + 1) : 0);
  const uint64_t end =
      out.compressed() ? 0 : indexEnd + uint64_t(out.pageCount) * out.pageBytes();
  if (indexEnd > UINT32_MAX || end > UINT32_MAX) return Error::SizeMismatch;
  out.pagesEnd = uint32_t(end);
  return Error::None;
}

Error validate(const Header &h, uint16_t pageW, uint16_t pageH, uint32_t fileSize) {
  if (h.pageW != pageW || h.pageH != pageH) return Error::WrongGeometry;
  if (h.pageCount == 0) return Error::NoPages;
  if (fileSize < (h.compressed() ? h.pagesOffset() : h.pagesEnd)) return Error::SizeMismatch;
  return Error::None;
}

Error readIndex(Reader &r, Header &h) {
  if (!h.compressed()) return Error::None;
  uint8_t b[4];
  if (!r.read(h.indexOffset(), b, sizeof(b))) return Error::TooShort;
  const uint32_t first = rd32(b);
  if (!r.read(h.indexOffset() + 4 * h.pageCount, b, sizeof(b))) return Error::TooShort;
  const uint32_t last = rd32(b);
  if (first != h.pagesOffset() || last < first || last - first < h.pageCount || last > r.size()) {
    return Error::BadIndex;
  }
  h.pagesEnd = last;
  return Error::None;
}

Error pageSpan(Reader &r, const Header &h, uint32_t n, uint32_t &offset, uint32_t &len) {
  if (n >= h.pageCount) return Error::BadPage;
  if (!h.compressed()) {
    offset = h.pageOffset(n);
    len = h.pageBytes();
    return Error::None;
  }
  uint8_t b[8];
  if (!r.read(h.indexOffset() + 4 * n, b, sizeof(b))) return Error::TooShort;
  const uint32_t from = rd32(b), to = rd32(b + 4);
  if (from < h.pagesOffset() || to <= from || to > h.pagesEnd) return Error::BadIndex;
  offset = from;
  len = to - from;
  return Error::None;
}

namespace {

// A stretch of the file as the inflater's input.
class Span : public zinf::Source {
 public:
  Span(Reader &r, uint32_t pos, uint32_t end) : r_(r), pos_(pos), end_(end) {}
  size_t pull(uint8_t *buf, size_t max) override {
    uint32_t n = end_ - pos_;
    if (n > max) n = uint32_t(max);
    if (n == 0 || !r_.read(pos_, buf, n)) return 0;
    pos_ += n;
    return n;
  }
  uint32_t left() const { return end_ - pos_; }

 private:
  Reader &r_;
  uint32_t pos_, end_;
};

}  // namespace

Error readPage(Reader &r, const Header &h, uint32_t n, uint8_t *dst, zinf::Inflater &inflater) {
  uint32_t offset = 0, len = 0;
  const Error err = pageSpan(r, h, n, offset, len);
  if (err != Error::None) return err;
  if (!h.compressed()) return r.read(offset, dst, len) ? Error::None : Error::TooShort;
  Span src(r, offset, offset + len);
  if (inflater.zlib(src, dst, h.pageBytes()) != zinf::Status::Ok) return Error::BadPage;
  // The stream must fill its stretch: one that ends early is another page's.
  return inflater.unused() == 0 && src.left() == 0 ? Error::None : Error::BadIndex;
}

Error parseCover(const Header &h, uint32_t fileSize, const uint8_t *head, size_t len, Cover &out) {
  out = Cover();
  const uint32_t end = h.pagesEnd;
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
