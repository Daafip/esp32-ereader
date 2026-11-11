// TF card and the /books library: books (one folder level), bookmarks, sleep
// pictures. A FireBeetle 7.5" without a card keeps the same files in its flash
// (kBooksInFlash: LittleFS). Layout on the card:
//   /books/*.t5b, /books/<folder>/*.t5b   books (file stems unique across folders)
//   /books/.src/<stem>.epub|.txt|.json    originals and converter settings (Restyle)
//   /books/.marks/<stem>.txt              bookmarks, one page number per line
//   /sleep/*.bin                          sleep pictures: full screen (kPageBytes) or
//                                         square beside the book (kSquareBytes)
//   /sleep/<screen id>/*.bin              the same for a turned screen (7.5" landscape)
//   /fonts/                               reading fonts added from the browser
#pragma once

#include <Arduino.h>
#include <FS.h>
#include <stdint.h>

#include <vector>

#include "bookmarks.h"
#include "t5b_format.h"

namespace storage {

constexpr const char *kBooksDir = "/books";
constexpr const char *kMarksDir = "/books/.marks";
constexpr const char *kPicturesDir = "/sleep";

enum class Status : uint8_t { Ok, NoCard, NoBooksDir, NoBooks };

struct BookEntry {
  String path;    // "/books/x.t5b" or "/books/Folder/x.t5b"; a folder: "/books/Folder"
  String folder;  // "" in the root
  String title;   // folders: the folder name
  String author;
  uint32_t pages = 0;  // folders: number of books in it
  uint32_t id = 0;     // t5b::bookId, for the reading position
  int screen = 0;      // the screen it was made for (index into kScreens)
  uint32_t size = 0;
  bool isFolder = false;
  bool hasCover = false;
};

// Mounts the card (tries 20, 10, 4 MHz), or the flash (formats it the first time).
// Safe to call again.
bool mount();
bool mounted();
// The card's (or the flash's) files: every path below is on it.
fs::FS &files();
// Size of the card in bytes, 0 if unknown.
uint64_t totalBytes();
// Free space on the card in bytes, or -1 if unknown. The first call after mounting
// may scan the FAT (a second or two on a big card); FatFs keeps the count after that.
// In the flash: what a new file can take (less a little for LittleFS's own blocks).
int64_t freeBytes();

// An open file as a t5b::Reader (page reads, index checks).
class FileReader : public t5b::Reader {
 public:
  explicit FileReader(fs::File &f) : f_(f) {}
  bool read(uint32_t offset, uint8_t *buf, uint32_t len) override;
  uint32_t size() override { return uint32_t(f_.size()); }

 private:
  fs::File &f_;
};

// Checks a header against the board's screens (two on boards that turn); `screen`
// gets the index of the one the book was made for.
t5b::Error checkHeader(const t5b::Header &h, uint32_t fileSize, int *screen = nullptr);
// Reads and checks a book's header, page index (version 2) and cover block, if any,
// for the board's screens.
t5b::Error checkBook(fs::File &f, t5b::Header &h, t5b::Cover *cover = nullptr,
                     int *screen = nullptr);

// One library level: folder "" = /books (its folders first, then its books),
// otherwise the books in /books/<folder>. Sorted by title. Invalid files are skipped;
// books for the board's other screen are listed (BookEntry::screen).
Status listLibrary(const char *folder, std::vector<BookEntry> &out, size_t max = 200);
// Every book in every folder (web page).
Status listAllBooks(std::vector<BookEntry> &out, size_t max = 400);

// Where a book of this file name ("x.t5b") is: "/books" or "/books/<folder>", or
// "/books" if there is none. A new copy (restyled, downloaded again) goes there.
String bookDirFor(const String &fileName);

// "Folder/x.t5b" -> "x" (bookmarks and originals are keyed by the stem).
String stemOf(const String &pathOrName);

void readMarks(const String &stem, uint32_t pageCount, PageList &out);
bool writeMarks(const String &stem, const PageList &marks);

// The sleep pictures of the screen shown now: /sleep, or /sleep/<screen id> on a
// turned screen (a page of the same size, the other shape).
String picturesDir();
// Sleep pictures of one shape (told apart by size) in picturesDir(): number of
// valid files, and the path of the i-th (sorted).
enum class PictureShape : uint8_t { Full, Square, Any };
int pictureCount(PictureShape shape);
String picturePath(PictureShape shape, int index);

// Deletes books, originals, bookmarks, pictures and fonts (factory reset).
void eraseAll();

}  // namespace storage
