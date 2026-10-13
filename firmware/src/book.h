// An open .t5b book on the card: header, TOC, page reads.
#pragma once

#include <FS.h>
#include <stdint.h>

#include <vector>

#include "t5b_format.h"

class Book {
 public:
  // Opens and validates; on failure error() says why.
  bool open(const char *path);
  void close();
  bool isOpen() const { return open_; }
  const char *error() const { return error_; }

  const char *path() const { return path_; }
  const t5b::Header &header() const { return header_; }
  uint32_t pageCount() const { return header_.pageCount; }
  uint32_t id() const { return id_; }

  // Reads page n (kPageBytes) into dst.
  bool readPage(uint32_t n, uint8_t *dst);

  // Optional cover image (bookprep / the web converter add it for EPUBs).
  const t5b::Cover &cover() const { return cover_; }
  // Reads the cover bitmap (cover().stride * cover().h bytes) into dst.
  bool readCover(uint8_t *dst);

  // "/books/Folder/x.t5b" -> "x": the key of bookmarks and originals.
  const char *stem() const { return stem_; }

  int tocCount() const { return int(starts_.size()); }
  uint32_t chapterStart(int i) const { return starts_[i]; }
  int chapterForPage(uint32_t page) const;
  bool tocEntry(int i, t5b::TocEntry &out);

 private:
  static constexpr int kMaxToc = 2048;  // 8 KB of start pages at most
  fs::File file_;
  t5b::Header header_;
  t5b::Cover cover_;
  std::vector<uint32_t> starts_;
  char path_[128] = {};
  char stem_[72] = {};
  const char *error_ = "";
  uint32_t id_ = 0;
  bool open_ = false;
};
