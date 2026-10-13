#include "bookmarks.h"

#include <stdio.h>

bool PageList::contains(uint32_t page) const {
  for (int i = 0; i < n_; i++) {
    if (pages_[i] == page) return true;
  }
  return false;
}

bool PageList::add(uint32_t page) {
  if (n_ >= kMax || contains(page)) return false;
  int i = n_++;
  for (; i > 0 && pages_[i - 1] > page; i--) pages_[i] = pages_[i - 1];
  pages_[i] = page;
  return true;
}

bool PageList::remove(uint32_t page) {
  for (int i = 0; i < n_; i++) {
    if (pages_[i] != page) continue;
    for (int j = i + 1; j < n_; j++) pages_[j - 1] = pages_[j];
    n_--;
    return true;
  }
  return false;
}

void PageList::parse(const char *text, size_t len, uint32_t pageCount) {
  clear();
  uint32_t v = 0;
  bool digits = false, junk = false;
  for (size_t i = 0; i <= len; i++) {
    const char c = i < len ? text[i] : '\n';
    if (c == '\n' || c == '\r') {
      if (digits && !junk && v < pageCount) add(v);
      v = 0;
      digits = junk = false;
    } else if (c >= '0' && c <= '9' && v < 100000000) {
      v = v * 10 + uint32_t(c - '0');
      digits = true;
    } else {
      junk = true;
    }
  }
}

size_t PageList::format(char *out, size_t outSize) const {
  size_t len = 0;
  if (outSize) out[0] = '\0';
  for (int i = 0; i < n_; i++) {
    const int w = snprintf(out + len, outSize - len, "%u\n", unsigned(pages_[i]));
    if (w < 0 || len + size_t(w) >= outSize) break;
    len += size_t(w);
  }
  return len;
}

void PageList::remap(uint32_t oldCount, uint32_t newCount) {
  PageList moved;
  for (int i = 0; i < n_; i++) moved.add(remapPage(pages_[i], oldCount, newCount));
  *this = moved;
}

uint32_t remapPage(uint32_t page, uint32_t oldCount, uint32_t newCount) {
  if (newCount == 0) return 0;
  if (oldCount <= 1 || newCount == 1) return 0;
  const uint64_t p = (uint64_t(page) * (newCount - 1) + (oldCount - 1) / 2) / (oldCount - 1);
  return p >= newCount ? newCount - 1 : uint32_t(p);
}
