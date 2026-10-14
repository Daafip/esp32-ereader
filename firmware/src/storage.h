// TF card and the /books library: books (one folder level), bookmarks, sleep
// pictures. Layout on the card:
//   /books/*.t5b, /books/<folder>/*.t5b   books (file stems unique across folders)
//   /books/.src/<stem>.epub|.txt|.json    originals and converter settings (Restyle)
//   /books/.marks/<stem>.txt              bookmarks, one page number per line
//   /sleep/*.bin                          sleep pictures (one page each, kPageBytes)
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
  uint32_t size = 0;
  bool isFolder = false;
  bool hasCover = false;
};

// Mounts the card (tries 20, 10, 4 MHz). Safe to call again.
bool mount();
bool mounted();
// Free space on the card in bytes, or -1 if unknown. The first call after mounting
// may scan the FAT (a second or two on a big card); FatFs keeps the count after that.
int64_t freeBytes();

// Reads and checks a book's header (and cover block, if any) for this screen.
t5b::Error checkBook(fs::File &f, t5b::Header &h, t5b::Cover *cover = nullptr);

// One library level: folder "" = /books (its folders first, then its books),
// otherwise the books in /books/<folder>. Sorted by title. Invalid files are skipped.
Status listLibrary(const char *folder, std::vector<BookEntry> &out, size_t max = 200);
// Every book in every folder (web page).
Status listAllBooks(std::vector<BookEntry> &out, size_t max = 400);

// "Folder/x.t5b" -> "x" (bookmarks and originals are keyed by the stem).
String stemOf(const String &pathOrName);

void readMarks(const String &stem, uint32_t pageCount, PageList &out);
bool writeMarks(const String &stem, const PageList &marks);

// Sleep pictures: number of valid files, and the path of the i-th (sorted).
int pictureCount();
String picturePath(int index);

// Deletes books, originals, bookmarks, pictures and fonts (factory reset).
void eraseAll();

}  // namespace storage
