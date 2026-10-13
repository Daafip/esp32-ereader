// Finds a board tag ("<prefix><name>", e.g. "T5READER-BOARD:m5paper") in a
// firmware image streamed in chunks, so a Wi-Fi update for another board can be
// refused before it is activated. The first occurrence counts. The prefix must
// not contain its own first character again (no partial-match fallback needed).
#pragma once

#include <stddef.h>
#include <stdint.h>

class BoardTagScanner {
 public:
  BoardTagScanner(const char *prefix, size_t prefixLen) : prefix_(prefix), prefixLen_(prefixLen) {}
  void reset();
  void feed(const uint8_t *data, size_t len);
  bool found() const { return done_ && nameLen_ > 0; }
  const char *name() const { return name_; }  // "" until found

 private:
  const char *prefix_;
  size_t prefixLen_;
  size_t matched_ = 0;
  bool inName_ = false;
  bool done_ = false;
  char name_[32] = "";
  size_t nameLen_ = 0;
};
