// .t5b book format (see docs/t5b-format.md). Parsing, and page reads through a
// Reader (the firmware's files, the PC's in ui_preview and the tests).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "inflater.h"

namespace t5b {

constexpr uint32_t kHeaderSize = 146;
constexpr uint32_t kTocEntrySize = 52;
constexpr size_t kTitleBytes = 64;
constexpr size_t kAuthorBytes = 64;
constexpr size_t kTocTitleBytes = 48;
constexpr uint16_t kVersion = 2;  // compressed pages and a page index
constexpr uint16_t kOldestVersion = 1;  // fixed-size raw pages, still read

struct Header {
  uint16_t version = 0;
  uint16_t pageW = 0;
  uint16_t pageH = 0;
  uint16_t stride = 0;
  uint32_t pageCount = 0;
  uint16_t tocCount = 0;
  char title[kTitleBytes + 1] = {};  // always NUL-terminated
  char author[kAuthorBytes + 1] = {};

  // End of the pages, where the optional cover starts. Version 1: from the header
  // (parseHeader); version 2: the last page index entry (readIndex).
  uint32_t pagesEnd = 0;

  bool compressed() const { return version >= 2; }
  uint32_t pageBytes() const { return uint32_t(stride) * pageH; }
  // Version 2: (pageCount + 1) u32 file offsets right after the TOC.
  uint32_t indexOffset() const { return kHeaderSize + uint32_t(tocCount) * kTocEntrySize; }
  uint32_t indexBytes() const { return compressed() ? 4 * (pageCount + 1) : 0; }
  // Where the first page's data starts.
  uint32_t pagesOffset() const { return indexOffset() + indexBytes(); }
  // Version 1 only: pages are pageBytes() each.
  uint32_t pageOffset(uint32_t n) const { return pagesOffset() + n * pageBytes(); }
};

struct TocEntry {
  uint32_t startPage = 0;
  char title[kTocTitleBytes + 1] = {};
};

enum class Error : uint8_t {
  None,
  TooShort,
  BadMagic,
  BadVersion,
  BadStride,
  WrongGeometry,
  NoPages,
  SizeMismatch,
  BadIndex,
  BadPage,
};

const char *errorText(Error e);

// Parses the 146 header bytes (magic, version, stride consistency).
Error parseHeader(const uint8_t *data, size_t len, Header &out);

// Checks a parsed header against the panel geometry and the real file size. The
// file may be longer than the pages: an optional cover block follows them
// (check it with parseCover()). Version 2: only that the index fits; readIndex()
// checks the rest.
Error validate(const Header &h, uint16_t pageW, uint16_t pageH, uint32_t fileSize);

// Random access to a book file.
class Reader {
 public:
  // Exactly len bytes at offset, or false.
  virtual bool read(uint32_t offset, uint8_t *buf, uint32_t len) = 0;
  virtual uint32_t size() = 0;

 protected:
  ~Reader() = default;
};

// After validate(): version 2 reads the first and last index entries, checks them
// and sets h.pagesEnd. Version 1 has nothing to read.
Error readIndex(Reader &r, Header &h);

// Page n (h.pageBytes()) into dst, after readIndex(). Version 2 inflates it with
// `inflater`: its data must be one zlib stream, exactly. On an error dst holds part
// of a page.
Error readPage(Reader &r, const Header &h, uint32_t n, uint8_t *dst, zinf::Inflater &inflater);

// Version 2: where page n's compressed data lies (a zlib stream), after readIndex().
Error pageSpan(Reader &r, const Header &h, uint32_t n, uint32_t &offset, uint32_t &len);

// Optional cover block right after the last page (docs/t5b-format.md):
// "T5CV", u16 width, u16 height, u16 stride, u16 reserved, then the 1-bpp bitmap
// (same bit layout as the pages), and nothing after it.
constexpr uint32_t kCoverHeaderSize = 12;
struct Cover {
  bool present = false;
  uint16_t w = 0, h = 0, stride = 0;
  uint32_t offset = 0;  // file offset of the bitmap
};
// `head`: the kCoverHeaderSize bytes at h.pagesEnd (nullptr if the file ends
// there). SizeMismatch if trailing data isn't a well-formed cover filling the file.
Error parseCover(const Header &h, uint32_t fileSize, const uint8_t *head, size_t len, Cover &out);

// Parses one 52-byte TOC entry.
void parseTocEntry(const uint8_t *data, TocEntry &out);

// Index of the last entry with starts[i] <= page (starts ascending), or -1.
int chapterForPage(const uint32_t *starts, int count, uint32_t page);

// Stable id for bookmarks: FNV-1a over the title and the page count.
uint32_t bookId(const char *title, uint32_t pageCount);

// NVS key for a book's bookmark: "b" + 8 hex digits (fits NVS's 15-char limit).
void bookmarkKey(uint32_t id, char out[10]);

}  // namespace t5b
