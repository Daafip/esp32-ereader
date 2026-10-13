#include "board_tag.h"

namespace {
bool nameChar(uint8_t c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}
}  // namespace

void BoardTagScanner::reset() {
  matched_ = nameLen_ = 0;
  inName_ = done_ = false;
  name_[0] = '\0';
}

void BoardTagScanner::feed(const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len && !done_; i++) {
    const uint8_t c = data[i];
    if (inName_) {
      if (nameChar(c) && nameLen_ < sizeof(name_) - 1) {
        name_[nameLen_++] = char(c);
        name_[nameLen_] = '\0';
        continue;
      }
      if (nameLen_ > 0) {
        done_ = true;
        break;
      }
      inName_ = false;  // prefix without a name: keep looking
      matched_ = 0;
    }
    if (c == uint8_t(prefix_[matched_])) {
      if (++matched_ == prefixLen_) inName_ = true;
    } else {
      matched_ = c == uint8_t(prefix_[0]) ? 1 : 0;
    }
  }
}
