#include "storage.h"

#include <SD.h>
#include <SPI.h>
#include <strings.h>

#include <algorithm>

#include "board.h"
#include "display.h"

namespace storage {
namespace {

SPIClass ownSpi(HSPI);
// M5Paper: the card shares the IT8951's bus, so both use Arduino's global SPI.
SPIClass &sdSpi = kSdSharedSpi ? SPI : ownSpi;
bool isMounted = false;

bool endsWith(const char *name, const char *ext) {
  const size_t n = strlen(name), e = strlen(ext);
  return n > e && strcasecmp(name + n - e, ext) == 0;
}

bool readEntry(fs::File &f, const String &dir, const String &folder, BookEntry &e) {
  t5b::Header h;
  t5b::Cover cover;
  const t5b::Error err = checkBook(f, h, &cover, &e.screen);
  if (err != t5b::Error::None) {
    Serial.printf("library: skipped %s/%s: %s\n", dir.c_str(), f.name(), t5b::errorText(err));
    return false;
  }
  e.path = dir + "/" + f.name();
  e.folder = folder;
  e.title = h.title[0] ? h.title : f.name();
  e.author = h.author;
  e.pages = h.pageCount;
  e.id = t5b::bookId(h.title, h.pageCount);
  e.size = uint32_t(f.size());
  e.hasCover = cover.present;
  return true;
}

uint32_t countBooks(const String &dir) {
  fs::File d = SD.open(dir);
  uint32_t n = 0;
  if (!d || !d.isDirectory()) return 0;
  for (fs::File f = d.openNextFile(); f; f = d.openNextFile()) {
    if (!f.isDirectory() && endsWith(f.name(), ".t5b")) n++;
  }
  return n;
}

bool byTitle(const BookEntry &a, const BookEntry &b) {
  if (a.isFolder != b.isFolder) return a.isFolder;
  return strcasecmp(a.title.c_str(), b.title.c_str()) < 0;
}

void removeTree(const String &path) {
  fs::File d = SD.open(path);
  if (!d) return;
  if (!d.isDirectory()) {
    d.close();
    SD.remove(path);
    return;
  }
  std::vector<String> children;
  for (fs::File f = d.openNextFile(); f; f = d.openNextFile()) {
    children.push_back(path + "/" + f.name());
  }
  d.close();
  for (const String &c : children) removeTree(c);
  SD.rmdir(path);
}

}  // namespace

bool mount() {
  if (isMounted) return true;
  sdSpi.begin(kSdSck, kSdMiso, kSdMosi, kSdCs);
  const uint32_t speeds[] = {kSdSpiHz, 10000000, 4000000};
  for (uint32_t hz : speeds) {
    if (SD.begin(kSdCs, sdSpi, hz)) {
      isMounted = true;
      Serial.printf("card: %u MB, type %d, %u MHz\n", unsigned(SD.cardSize() >> 20),
                    int(SD.cardType()), unsigned(hz / 1000000));
      return true;
    }
    SD.end();
  }
  Serial.println("card: not found");
  return false;
}

bool mounted() { return isMounted; }

int64_t freeBytes() {
  const uint64_t total = isMounted ? SD.totalBytes() : 0;
  return total ? int64_t(total - SD.usedBytes()) : -1;
}

t5b::Error checkHeader(const t5b::Header &h, uint32_t fileSize, int *screen) {
  t5b::Error first = t5b::Error::None;
  for (int i = 0; i < kScreenCount; i++) {
    const t5b::Error err = t5b::validate(h, kScreens[i].w, kScreens[i].h, fileSize);
    if (err == t5b::Error::None) {
      if (screen) *screen = i;
      return err;
    }
    if (i == 0) first = err;  // why it doesn't fit the board's own screen
  }
  return first;
}

t5b::Error checkBook(fs::File &f, t5b::Header &h, t5b::Cover *cover, int *screen) {
  uint8_t buf[t5b::kHeaderSize];
  if (!f.seek(0) || f.read(buf, sizeof(buf)) != sizeof(buf)) return t5b::Error::TooShort;
  t5b::Error err = t5b::parseHeader(buf, sizeof(buf), h);
  const uint32_t size = uint32_t(f.size());
  if (err == t5b::Error::None) err = checkHeader(h, size, screen);
  if (err != t5b::Error::None) return err;
  t5b::Cover c;
  uint8_t head[t5b::kCoverHeaderSize];
  const bool more = size > h.fileSize();
  if (more && !(f.seek(h.fileSize()) && f.read(head, sizeof(head)) == sizeof(head))) {
    return t5b::Error::SizeMismatch;
  }
  err = t5b::parseCover(h, size, more ? head : nullptr, sizeof(head), c);
  if (cover) *cover = c;
  return err;
}

Status listLibrary(const char *folder, std::vector<BookEntry> &out, size_t max) {
  out.clear();
  if (!isMounted) return Status::NoCard;
  const String dir = *folder ? String(kBooksDir) + "/" + folder : String(kBooksDir);
  fs::File d = SD.open(dir);
  if (!d || !d.isDirectory()) {
    Serial.printf("library: no folder %s on the card\n", dir.c_str());
    return Status::NoBooksDir;
  }
  for (fs::File f = d.openNextFile(); f && out.size() < max; f = d.openNextFile()) {
    if (f.isDirectory()) {
      if (*folder || f.name()[0] == '.') continue;  // one level; .src, .marks
      BookEntry e;
      e.isFolder = true;
      e.title = f.name();
      e.path = dir + "/" + f.name();
      e.pages = countBooks(e.path);
      if (e.pages) out.push_back(e);
      continue;
    }
    BookEntry e;
    if (endsWith(f.name(), ".t5b") && readEntry(f, dir, folder, e)) out.push_back(e);
  }
  std::sort(out.begin(), out.end(), byTitle);
  Serial.printf("library: %u entries in %s\n", unsigned(out.size()), dir.c_str());
  return out.empty() ? Status::NoBooks : Status::Ok;
}

Status listAllBooks(std::vector<BookEntry> &out, size_t max) {
  std::vector<BookEntry> level;
  const Status s = listLibrary("", level, max);
  out.clear();
  for (const BookEntry &e : level) {
    if (!e.isFolder) {
      out.push_back(e);
      continue;
    }
    std::vector<BookEntry> inner;
    listLibrary(e.title.c_str(), inner, max);
    for (const BookEntry &b : inner) {
      if (out.size() < max) out.push_back(b);
    }
  }
  if (!out.empty()) return Status::Ok;
  return s == Status::NoCard || s == Status::NoBooksDir ? s : Status::NoBooks;
}

String stemOf(const String &pathOrName) {
  String name = pathOrName.substring(pathOrName.lastIndexOf('/') + 1);
  const int dot = name.lastIndexOf('.');
  return dot > 0 ? name.substring(0, dot) : name;
}

void readMarks(const String &stem, uint32_t pageCount, PageList &out) {
  out.clear();
  fs::File f = SD.open(String(kMarksDir) + "/" + stem + ".txt", FILE_READ);
  if (!f) return;
  char buf[1200];
  const size_t n = f.read(reinterpret_cast<uint8_t *>(buf), sizeof(buf));
  f.close();
  out.parse(buf, n, pageCount);
}

bool writeMarks(const String &stem, const PageList &marks) {
  const String path = String(kMarksDir) + "/" + stem + ".txt";
  if (marks.count() == 0) {
    SD.remove(path);
    return true;
  }
  SD.mkdir(kBooksDir);
  SD.mkdir(kMarksDir);
  char buf[1200];
  const size_t n = marks.format(buf, sizeof(buf));
  fs::File f = SD.open(path, FILE_WRITE);
  if (!f) return false;
  const bool ok = f.write(reinterpret_cast<const uint8_t *>(buf), n) == n;
  f.close();
  return ok;
}

namespace {
bool hasShape(size_t size, PictureShape shape) {
  const bool full = size == size_t(kPageBytes), square = size == size_t(kSquareBytes);
  switch (shape) {
    case PictureShape::Full: return full;
    case PictureShape::Square: return square;
    case PictureShape::Any: return full || square;
  }
  return false;
}

void pictureNames(PictureShape shape, std::vector<String> &names) {
  names.clear();
  fs::File d = SD.open(picturesDir());
  if (!d || !d.isDirectory()) return;
  for (fs::File f = d.openNextFile(); f && names.size() < 100; f = d.openNextFile()) {
    if (!f.isDirectory() && endsWith(f.name(), ".bin") && hasShape(f.size(), shape)) {
      names.push_back(f.name());
    }
  }
  std::sort(names.begin(), names.end(), [](const String &a, const String &b) { return a < b; });
}
}  // namespace

String picturesDir() {
  const int screen = display::screenIndex();
  if (screen <= 0 || screen >= kScreenCount) return kPicturesDir;
  return String(kPicturesDir) + "/" + kScreens[screen].id;
}

int pictureCount(PictureShape shape) {
  std::vector<String> names;
  pictureNames(shape, names);
  return int(names.size());
}

String picturePath(PictureShape shape, int index) {
  std::vector<String> names;
  pictureNames(shape, names);
  if (index < 0 || index >= int(names.size())) return "";
  return picturesDir() + "/" + names[index];
}

void eraseAll() {
  removeTree(kBooksDir);
  removeTree(kPicturesDir);
  removeTree("/fonts");
}

}  // namespace storage
