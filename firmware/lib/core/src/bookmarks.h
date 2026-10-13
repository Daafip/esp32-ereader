// Bookmarks of one book: a sorted list of distinct pages, stored on the card as
// text (one page number per line, /books/.marks/<book stem>.txt).
#pragma once

#include <stddef.h>
#include <stdint.h>

class PageList {
 public:
  static constexpr int kMax = 100;

  int count() const { return n_; }
  uint32_t at(int i) const { return pages_[i]; }
  bool contains(uint32_t page) const;
  bool add(uint32_t page);  // false if present or full
  bool remove(uint32_t page);
  void clear() { n_ = 0; }

  // "12\n345\n": ignores junk lines, duplicates and pages >= pageCount.
  void parse(const char *text, size_t len, uint32_t pageCount);
  size_t format(char *out, size_t outSize) const;  // returns the length

  // After a restyle the book has another page count: move every mark in proportion.
  void remap(uint32_t oldCount, uint32_t newCount);

 private:
  uint32_t pages_[kMax];
  int n_ = 0;
};

// A page position carried from a book of oldCount pages to one of newCount pages
// (restyled), in proportion, rounded.
uint32_t remapPage(uint32_t page, uint32_t oldCount, uint32_t newCount);
