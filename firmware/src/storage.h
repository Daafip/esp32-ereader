// TF card on its own SPI bus (HSPI) and the /books library.
#pragma once

#include <Arduino.h>
#include <stdint.h>

#include <vector>

namespace storage {

constexpr const char *kBooksDir = "/books";

enum class Status : uint8_t { Ok, NoCard, NoBooksDir, NoBooks };

struct BookEntry {
  String path;
  String title;
  String author;
  uint32_t pages = 0;
  uint32_t id = 0;  // t5b::bookId, for the bookmark
};

// Mounts the card (tries 20, 10, 4 MHz). Safe to call again.
bool mount();
bool mounted();
uint32_t cardSizeMb();

// /books/*.t5b with title/author from their headers, sorted by title.
// Files that aren't valid books for this screen are skipped.
Status listBooks(std::vector<BookEntry> &out, size_t max = 64);

}  // namespace storage
