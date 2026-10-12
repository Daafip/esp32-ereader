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
#include "web_util.h"

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

#define DOT "  \xC2\xB7  "
const ui::Hint kNext = {"NEXT", ui::kArrowDown, ui::kArrowUp};
const ui::Hint kOpen = {"MENU", "open", "back"};
const ui::Hint kGo = {"MENU", "go", "back"};

struct Row {
  const char *label;
  const char *detail;
  bool chevron;
  bool current;
};

void rowItem(int i, ui::Item &it, void *ctx) {
  const Row &r = static_cast<const Row *>(ctx)[i];
  strncpy(it.label, r.label, sizeof(it.label) - 1);
  strncpy(it.detail, r.detail, sizeof(it.detail) - 1);
  it.chevron = r.chevron;
  it.current = r.current;
}

const Row kMain[] = {
    {"Resume", "1%", false, false},        {"Chapters", "", true, false},
    {"Go to position", "", true, false},   {"Library", "", true, false},
    {"Settings", "", true, false},         {"About", "", true, false},
    {"Sleep", "", false, false},
};
const Row kChapters[] = {
    {"OF DE", "6", false, false},
    {"AAN DE", "8", false, false},
    {"EERSTE HOOFDSTUK", "26", false, true},
    {"TWEEDE HOOFDSTUK", "82", false, false},
    {"DERDE HOOFDSTUK", "151", false, false},
    {"VIERDE HOOFDSTUK", "217", false, false},
    {"VYFDE HOOFDSTUK", "394", false, false},
    {"ZESDE HOOFDSTUK", "553", false, false},
    {"ZEVENDE HOOFDSTUK", "712", false, false},
    {"ACHTSTE HOOFDSTUK", "898", false, false},
};
const Row kBooks[] = {
    {"Alice's Adventures in Wonderland", "new", false, false},
    {"Max Havelaar / Of de koffiveilingen der Nederlandsche Handelsmaatschappy", "1%", false, true},
    {"De Kleine Johannes", "64%", false, false},
    {"Een Nederlandsch \xE2\x80\x9C" "ABC\xE2\x80\x9D voor \xC3\xA9\xC3\xA9n en al", "100%", false,
     false},
};

void settingItem(int i, ui::Item &it, void *) {
  Settings s;
  strncpy(it.label, settingLabel(SettingId(i)), sizeof(it.label) - 1);
  settingValue(s, SettingId(i), it.detail, sizeof(it.detail));
}

}  // namespace

int main(int argc, char **argv) {
  if (argc > 1) outDir = argv[1];
  display::begin();
  ListModel list;
  const char *title = "Max Havelaar / Of de koffiveilingen der Nederlandsche Handelsmaatschappy";

  memcpy(display::framebuffer(), kSamplePages[0], kPageBytes);
  ui::statusBar("EERSTE HOOFDSTUK", 26, 3596, 72);
  save("01-reading");

  memcpy(display::framebuffer(), kSamplePages[1], kPageBytes);
  ui::statusBar("Hoofdstuk \xC3\xA9\xC3\xA9n \xE2\x80\x94 De koffiveilingen der Nederlandsche "
                "Handelsmaatschappy",
                2400, 3596, 8);
  save("02-reading-long-title");

  list.reset(7, ui::kMenuRows, 0);
  ui::menu(title, list, rowItem, (void *)kMain, kNext, kOpen, 72);
  save("03-main-menu");

  list.reset(10, ui::kMenuRows, 2);
  ui::menu("Chapters", list, rowItem, (void *)kChapters, kNext, kGo, 72);
  save("04-chapters");

  ui::valueEditor("Go to position", "37%", "Page 1330 of 3596", 37, {"NEXT", "+1", "+10"}, kGo,
                  72);
  save("05-go-to");

  list.reset(4, ui::kMenuRows, 1);
  ui::menu("Library", list, rowItem, (void *)kBooks, kNext, kOpen, 72);
  save("06-library");

  list.reset(kSettingCount, ui::kMenuRows, 4);
  ui::menu("Settings", list, settingItem, nullptr, kNext, {"MENU", "edit", "back"}, 72);
  save("07-settings");

  const char *labels[] = {"Firmware", "Battery", "Book", "Position", "Memory", "Wake to page"};
  const char *values[] = {"0.6.0", "3.92 V" DOT "72%", title, "page 27/3596" DOT "ch. 3/25",
                          "7580 MB card" DOT "214 KB free", "812 ms" DOT "1532 wakes"};
  ui::details("About", labels, values, 6, {nullptr, nullptr, nullptr}, {"MENU", "back", nullptr},
              72);
  save("08-about");

  ui::message(ui::Icon::Card, "No SD card", "Insert a FAT32 card", "with books in /books",
              "Press any button to try again");
  save("09-no-card");

  ui::message(ui::Icon::Book, "No books yet", "Upload them over Wi-Fi, or copy",
              ".t5b files to /books on the card", {"MENU", "Wi-Fi transfer", nullptr},
              {"NEXT", "retry", nullptr});
  save("10-no-books");

  ui::message(ui::Icon::Warning, "Cannot open book", "old.t5b", "made for another screen size",
              "Press any button to try again");
  save("11-error");

  ui::sleepScreen(title, "Multatuli", 37, 72);
  save("12-sleep");

  ui::sleepScreen("De Kleine Johannes", "Frederik van Eeden", 64, 15);
  save("13-sleep-short-title");

  ui::message(ui::Icon::Battery, "Battery low", "Charge the reader,", "then press a button",
              nullptr);
  save("14-low-battery");

  char join[96];
  wifiQrPayload("T5-Reader-1A2B", "k7m4q9xp", join, sizeof(join));
  ui::wifiScreen({join, 1, "Join the Wi-Fi", "Network", "T5-Reader-1A2B", "Password", "k7m4q9xp", ""},
                 {"NEXT", "show page", nullptr}, {"MENU", "done", nullptr}, 72);
  save("15-wifi-join");

  ui::wifiScreen({"http://192.168.4.1/?k=4821", 2, "Open the page", "Address", "192.168.4.1",
                  "Code", "4821", "Received Max Havelaar"},
                 {"NEXT", "show Wi-Fi", nullptr}, {"MENU", "done", nullptr}, 72);
  save("16-wifi-page");
  return 0;
}
