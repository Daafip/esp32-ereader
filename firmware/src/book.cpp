#include "book.h"

#include <SD.h>

#include "board.h"
#include "storage.h"

bool Book::open(const char *path) {
  close();
  strlcpy(path_, path, sizeof(path_));
  file_ = SD.open(path, FILE_READ);
  if (!file_) {
    error_ = "file not found";
    return false;
  }
  const t5b::Error err = storage::checkBook(file_, header_, &cover_, &screen_);
  if (err != t5b::Error::None || !file_.seek(t5b::kHeaderSize)) {
    error_ = t5b::errorText(err);
    file_.close();
    return false;
  }
  strlcpy(stem_, storage::stemOf(path).c_str(), sizeof(stem_));
  const int n = header_.tocCount < kMaxToc ? header_.tocCount : kMaxToc;
  starts_.resize(n);
  uint8_t entry[t5b::kTocEntrySize];
  for (int i = 0; i < n; i++) {
    if (file_.read(entry, sizeof(entry)) != sizeof(entry)) {
      starts_.resize(i);
      break;
    }
    starts_[i] = uint32_t(entry[0]) | (uint32_t(entry[1]) << 8) | (uint32_t(entry[2]) << 16) |
                 (uint32_t(entry[3]) << 24);
  }
  id_ = t5b::bookId(header_.title, header_.pageCount);
  error_ = "";
  open_ = true;
  return true;
}

void Book::close() {
  if (file_) file_.close();
  starts_.clear();
  open_ = false;
}

bool Book::readPage(uint32_t n, uint8_t *dst) {
  if (!open_ || n >= header_.pageCount) return false;
  const uint32_t bytes = header_.pageBytes();
  return file_.seek(header_.pageOffset(n)) && file_.read(dst, bytes) == bytes;
}

bool Book::readCover(uint8_t *dst) {
  if (!open_ || !cover_.present) return false;
  const uint32_t bytes = uint32_t(cover_.stride) * cover_.h;
  return file_.seek(cover_.offset) && file_.read(dst, bytes) == bytes;
}

int Book::chapterForPage(uint32_t page) const {
  return t5b::chapterForPage(starts_.data(), int(starts_.size()), page);
}

bool Book::tocEntry(int i, t5b::TocEntry &out) {
  if (!open_ || i < 0 || i >= int(starts_.size())) return false;
  uint8_t entry[t5b::kTocEntrySize];
  if (!file_.seek(t5b::kHeaderSize + uint32_t(i) * t5b::kTocEntrySize) ||
      file_.read(entry, sizeof(entry)) != sizeof(entry)) {
    return false;
  }
  t5b::parseTocEntry(entry, out);
  return true;
}
