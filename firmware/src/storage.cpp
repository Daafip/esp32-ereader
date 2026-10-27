#include "storage.h"

#include <strings.h>

#include <algorithm>

#include "board.h"
#include "display.h"

#ifdef BOARD_BOOKS_IN_FLASH
#include <LittleFS.h>
#else
#include <SD.h>
#include <SPI.h>
#endif

namespace storage {
namespace {

#ifdef BOARD_BOOKS_IN_FLASH
// The partition for the books (firmware/partitions-fb75-flash.csv).
constexpr const char *kFlashLabel = "books";
#else
SPIClass ownSpi(HSPI);
// M5Paper: the card shares the IT8951's bus, so both use Arduino's global SPI.
SPIClass &sdSpi = kSdSharedSpi ? SPI : ownSpi;
#endif
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
  fs::File d = files().open(dir);
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
  fs::File d = files().open(path);
  if (!d) return;
  if (!d.isDirectory()) {
    d.close();
    files().remove(path);
    return;
  }
  std::vector<String> children;
  for (fs::File f = d.openNextFile(); f; f = d.openNextFile()) {
    children.push_back(path + "/" + f.name());
  }
  d.close();
  for (const String &c : children) removeTree(c);
  files().rmdir(path);
}

}  // namespace

#ifdef BOARD_BOOKS_IN_FLASH
bool mount() {
  if (isMounted) return true;
  // Formats the partition when it holds no file system: the first start after
  // flashing this partition table (a few seconds).
  isMounted = LittleFS.begin(true, "/littlefs", 10, kFlashLabel);
  if (isMounted) {
    Serial.printf("flash: %u KB, %u KB free\n", unsigned(LittleFS.totalBytes() >> 10),
                  unsigned((LittleFS.totalBytes() - LittleFS.usedBytes()) >> 10));
  } else {
    Serial.printf("flash: no partition \"%s\" (flash this env over USB)\n", kFlashLabel);
  }
  return isMounted;
}

fs::FS &files() { return LittleFS; }

uint64_t totalBytes() { return isMounted ? LittleFS.totalBytes() : 0; }

int64_t freeBytes() {
  if (!isMounted) return -1;
  // LittleFS counts whole 4 KB blocks, and a file also needs a few for its block
  // list and the folder's metadata: keep 16 KB and 1/256 back.
  const uint64_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
  const uint64_t free = total > used ? total - used : 0;
  const uint64_t slack = 16384 + free / 256;
  return free > slack ? int64_t(free - slack) : 0;
}
#else
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

fs::FS &files() { return SD; }

uint64_t totalBytes() { return isMounted ? SD.totalBytes() : 0; }

int64_t freeBytes() {
  const uint64_t total = isMounted ? SD.totalBytes() : 0;
  return total ? int64_t(total - SD.usedBytes()) : -1;
}
#endif

bool mounted() { return isMounted; }

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
  fs::File d = files().open(dir);
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
  fs::File f = files().open(String(kMarksDir) + "/" + stem + ".txt", FILE_READ);
  if (!f) return;
  char buf[1200];
  const size_t n = f.read(reinterpret_cast<uint8_t *>(buf), sizeof(buf));
  f.close();
  out.parse(buf, n, pageCount);
}

bool writeMarks(const String &stem, const PageList &marks) {
  const String path = String(kMarksDir) + "/" + stem + ".txt";
  if (marks.count() == 0) {
    files().remove(path);
    return true;
  }
  files().mkdir(kBooksDir);
  files().mkdir(kMarksDir);
  char buf[1200];
  const size_t n = marks.format(buf, sizeof(buf));
  fs::File f = files().open(path, FILE_WRITE);
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
  fs::File d = files().open(picturesDir());
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
