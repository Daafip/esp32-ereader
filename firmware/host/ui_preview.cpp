// Renders every UI screen with the real ui.cpp into PBM files:
//   ui_preview <out_dir>
// Driven by `pixi run ui-preview`, which also converts them to PNG.
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "display.h"
#include "menu.h"
#include "settings.h"
#include "test_pages.h"
#include "ui.h"

namespace {

const char *outDir = ".";

void save(const char *name) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s.pbm", outDir, name);
  FILE *f = fopen(path, "wb");
  if (!f) {
    perror(path);
    return;
  }
  fprintf(f, "P4\n%d %d\n", kPageW, kPageH);
  const uint8_t *fb = display::framebuffer();
  for (int i = 0; i < kPageBytes; i++) fputc(fb[i] ^ 0xFF, f);  // PBM: 1 = black
  fclose(f);
  printf("%s\n", path);
}

const char *const kMain[] = {"Resume", "Chapters", "Go to %", "Library", "Settings", "About", "Sleep"};
const char *const kChapters[] = {
    "OF DE", "AAN DE", "EERSTE HOOFDSTUK", "TWEEDE HOOFDSTUK", "DERDE HOOFDSTUK",
    "VIERDE HOOFDSTUK", "VYFDE HOOFDSTUK", "\xE2\x80\xA2 ZESDE HOOFDSTUK", "ZEVENDE HOOFDSTUK",
    "ACHTSTE HOOFDSTUK", "NEGENDE HOOFDSTUK", "TIENDE HOOFDSTUK",
};
const char *const kBooks[] = {
    "Alice's Adventures in Wonderland",
    "\xE2\x80\xA2 Max Havelaar / Of de koffiveilingen der Nederlandsche Handelsmaatschappy",
    "De Kleine Johannes", "Een Nederlandsch \xE2\x80\x9C" "ABC\xE2\x80\x9D voor \xC3\xA9\xC3\xA9n en al",
};

void strings(int i, char *out, size_t n, void *ctx) {
  strncpy(out, static_cast<const char *const *>(ctx)[i], n - 1);
  out[n - 1] = '\0';
}

void settingText(int i, char *out, size_t n, void *) {
  Settings s;
  describeSetting(s, SettingId(i), out, n);
}

}  // namespace

int main(int argc, char **argv) {
  if (argc > 1) outDir = argv[1];
  display::begin();
  const char *hint = "NEXT:down hold:up  MENU:ok hold:back";
  ListModel list;

  memcpy(display::framebuffer(), kSamplePages[0], kPageBytes);
  ui::statusBar("EERSTE HOOFDSTUK", 26, 3596, 72);
  save("01-reading");

  memcpy(display::framebuffer(), kSamplePages[1], kPageBytes);
  ui::statusBar("Hoofdstuk \xC3\xA9\xC3\xA9n \xE2\x80\x94 De koffiveilingen der Nederlandsche Handelsmaatschappy",
                3595, 3596, 8);
  save("02-reading-long-title");

  list.reset(7, ui::kMenuRows, 1);
  ui::menu("Max Havelaar / Of de koffiveilingen der Nederlandsche Handelsmaatschappy", list,
           strings, (void *)kMain, hint, 72);
  save("03-main-menu");

  list.reset(12, ui::kMenuRows, 7);
  ui::menu("Chapters", list, strings, (void *)kChapters, "p553  NEXT:down hold:up MENU:go", 72);
  save("04-chapters");

  ui::valueEditor("Go to", "37%  (p 1330)", 37, "NEXT:+1 hold:+10  MENU:go hold:back");
  save("05-go-to");

  list.reset(4, ui::kMenuRows, 1);
  ui::menu("Library", list, strings, (void *)kBooks, "Multatuli  3596p", 72);
  save("06-library");

  list.reset(kSettingCount, ui::kMenuRows, 4);
  ui::menu("Settings", list, settingText, nullptr, "NEXT:down hold:up  MENU:change hold:back", 72);
  save("07-settings");

  const char *about[] = {"Firmware 0.5.0, bb_epaper 2.1.11+", "Battery 3.92 V (72%)",
                         "Max Havelaar / Of de koffiveilingen der Nederlandsche Handelsmaatschappy",
                         "Page 27 of 3596, chapter 3 of 25", "SD 7580 MB, free heap 214 KB",
                         "Wake to image 812 ms, wakes 1532"};
  ui::textScreen("About", about, 6, "any button: back");
  save("08-about");

  ui::message("No SD card", "Insert a FAT32 card", "with books in /books", "any button: try again");
  save("09-message");

  ui::sleepScreen("Max Havelaar / Of de koffiveilingen der Nederlandsche Handelsmaatschappy",
                  "Multatuli", 72);
  save("10-sleep");

  ui::message("Battery low", "Please charge the reader", nullptr, "");
  save("11-low-battery");
  return 0;
}
