// .t5b book format (see docs/t5b-format.md). Pure parsing, no file I/O.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace t5b {

constexpr uint32_t kHeaderSize = 146;
constexpr uint32_t kTocEntrySize = 52;
constexpr size_t kTitleBytes = 64;
constexpr size_t kAuthorBytes = 64;
constexpr size_t kTocTitleBytes = 48;
constexpr uint16_t kVersion = 1;

struct Header {
  uint16_t version = 0;
  uint16_t pageW = 0;
  uint16_t pageH = 0;
  uint16_t stride = 0;
  uint32_t pageCount = 0;
  uint16_t tocCount = 0;
  char title[kTitleBytes + 1] = {};  // always NUL-terminated
  char author[kAuthorBytes + 1] = {};

  uint32_t pageBytes() const { return uint32_t(stride) * pageH; }
  uint32_t pagesOffset() const { return kHeaderSize + uint32_t(tocCount) * kTocEntrySize; }
  uint32_t pageOffset(uint32_t n) const { return pagesOffset() + n * pageBytes(); }
  uint32_t fileSize() const { return pageOffset(pageCount); }
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
};

const char *errorText(Error e);

// Parses the 146 header bytes (magic, version, stride consistency).
Error parseHeader(const uint8_t *data, size_t len, Header &out);

// Checks a parsed header against the panel geometry and the real file size.
Error validate(const Header &h, uint16_t pageW, uint16_t pageH, uint32_t fileSize);

// Parses one 52-byte TOC entry.
void parseTocEntry(const uint8_t *data, TocEntry &out);

// Index of the last entry with starts[i] <= page (starts ascending), or -1.
int chapterForPage(const uint32_t *starts, int count, uint32_t page);

// Stable id for bookmarks: FNV-1a over the title and the page count.
uint32_t bookId(const char *title, uint32_t pageCount);

// NVS key for a book's bookmark: "b" + 8 hex digits (fits NVS's 15-char limit).
void bookmarkKey(uint32_t id, char out[10]);

}  // namespace t5b
