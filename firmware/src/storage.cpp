#include "storage.h"

#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <strings.h>

#include <algorithm>

#include "board.h"
#include "t5b_format.h"

namespace storage {
namespace {

SPIClass sdSpi(HSPI);
bool isMounted = false;

bool endsWithT5b(const char *name) {
  const size_t n = strlen(name);
  return n > 4 && strcasecmp(name + n - 4, ".t5b") == 0;
}

}  // namespace

bool mount() {
  if (isMounted) return true;
  sdSpi.begin(kSdSck, kSdMiso, kSdMosi, kSdCs);
  const uint32_t speeds[] = {kSdSpiHz, 10000000, 4000000};
  for (uint32_t hz : speeds) {
    if (SD.begin(kSdCs, sdSpi, hz)) {
      isMounted = true;
      return true;
    }
    SD.end();
  }
  return false;
}

bool mounted() { return isMounted; }

uint32_t cardSizeMb() { return isMounted ? uint32_t(SD.cardSize() >> 20) : 0; }

Status listBooks(std::vector<BookEntry> &out, size_t max) {
  out.clear();
  if (!isMounted) return Status::NoCard;
  File dir = SD.open(kBooksDir);
  if (!dir || !dir.isDirectory()) return Status::NoBooksDir;
  uint8_t buf[t5b::kHeaderSize];
  for (File f = dir.openNextFile(); f && out.size() < max; f = dir.openNextFile()) {
    if (f.isDirectory() || !endsWithT5b(f.name())) continue;
    t5b::Header h;
    const bool ok = f.read(buf, sizeof(buf)) == sizeof(buf) &&
                    t5b::parseHeader(buf, sizeof(buf), h) == t5b::Error::None &&
                    t5b::validate(h, kPageW, kPageH, uint32_t(f.size())) == t5b::Error::None;
    if (!ok) continue;
    BookEntry e;
    e.path = String(kBooksDir) + "/" + f.name();
    e.title = h.title[0] ? h.title : f.name();
    e.author = h.author;
    e.pages = h.pageCount;
    out.push_back(e);
  }
  std::sort(out.begin(), out.end(), [](const BookEntry &a, const BookEntry &b) {
    return strcasecmp(a.title.c_str(), b.title.c_str()) < 0;
  });
  return out.empty() ? Status::NoBooks : Status::Ok;
}

}  // namespace storage
