// Screens drawn into the display framebuffer (call display::show() afterwards).
//
// Layout on the 250x122 landscape panel:
//   reading  page bitmap from the book, status bar in the bottom 10 rows
//   menus    16 px title bar, up to 6 rows of 15 px, footer hint (6x8 font)
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "menu.h"

namespace ui {

constexpr int kMenuRows = 6;

// Fills `out` with the text of list item `index`.
using ItemText = void (*)(int index, char *out, size_t outSize, void *ctx);

// Status bar over the bottom rows of a book page. batteryPct < 0 = unknown.
void statusBar(const char *chapter, uint32_t page, uint32_t pageCount, int batteryPct);

void menu(const char *title, const ListModel &model, ItemText itemText, void *ctx,
          const char *footer, int batteryPct);

// Centred message with up to two lines and a footer hint.
void message(const char *title, const char *line1, const char *line2, const char *footer);

// Title + lines of plain text (About screen).
void textScreen(const char *title, const char *const *lines, int count, const char *footer);

// Large value with a progress bar (Go to %).
void valueEditor(const char *title, const char *value, int percent, const char *footer);

void sleepScreen(const char *title, const char *author, int batteryPct);

}  // namespace ui
