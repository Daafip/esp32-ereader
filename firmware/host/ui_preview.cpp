// Renders every UI screen with the real ui.cpp into PBM files:
//   ui_preview <out_dir> <sample.t5b> <square-picture.bin> [orientation]
// The last two pages of the .t5b (made for this board's screen) are the book
// pages behind the status bar. Built once per board by `pixi run ui-preview`,
// which also converts the PBMs to PNG.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "board.h"
#include "display.h"
#include "i18n.h"
#include "menu.h"
#include "settings.h"
#include "t5b_format.h"
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
  fprintf(f, "P4\n%d %d\n", display::screen().w, display::screen().h);
  const uint8_t *fb = display::framebuffer();
  for (int i = 0; i < kPageBytes; i++) fputc(fb[i] ^ 0xFF, f);  // PBM: 1 = black
  fclose(f);
  printf("%s\n", path);
}

// A PC file as a t5b::Reader.
class FileReader : public t5b::Reader {
 public:
  explicit FileReader(FILE *f) : f_(f) {}
  bool read(uint32_t offset, uint8_t *buf, uint32_t len) override {
    return fseek(f_, long(offset), SEEK_SET) == 0 && fread(buf, 1, len, f_) == len;
  }
  uint32_t size() override {
    fseek(f_, 0, SEEK_END);
    return uint32_t(ftell(f_));
  }

 private:
  FILE *f_;
};

// Header and page index of the sample .t5b.
bool readHeader(FILE *f, t5b::Header &h) {
  FileReader r(f);
  uint8_t raw[t5b::kHeaderSize];
  return r.read(0, raw, sizeof(raw)) && t5b::parseHeader(raw, sizeof(raw), h) == t5b::Error::None &&
         t5b::validate(h, h.pageW, h.pageH, r.size()) == t5b::Error::None &&
         t5b::readIndex(r, h) == t5b::Error::None;
}

// Book page n of the sample .t5b (counted from the end) into the framebuffer.
bool loadPage(const char *path, int fromEnd) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    perror(path);
    return false;
  }
  t5b::Header h;
  static zinf::Inflater inflater;
  FileReader r(f);
  const bool ok = readHeader(f, h) && h.pageW == display::screen().w &&
                  h.pageH == display::screen().h && h.pageCount > uint32_t(fromEnd) &&
                  t5b::readPage(r, h, h.pageCount - 1 - fromEnd, display::framebuffer(),
                                inflater) == t5b::Error::None;
  fclose(f);
  if (!ok) {
    fprintf(stderr, "%s: no %dx%d sample page\n", path, display::screen().w,
            display::screen().h);
  }
  return ok;
}

// The cover block of the sample .t5b (bookprep's made-up cover), if any.
std::vector<uint8_t> coverBits;
ui::Bitmap loadCover(const char *path) {
  FILE *f = fopen(path, "rb");
  ui::Bitmap b;
  if (!f) return b;
  t5b::Header h;
  t5b::Cover c;
  uint8_t head[t5b::kCoverHeaderSize];
  FileReader r(f);
  const uint32_t size = r.size();
  if (readHeader(f, h) && r.read(h.pagesEnd, head, sizeof(head)) &&
      t5b::parseCover(h, size, head, sizeof(head), c) == t5b::Error::None && c.present) {
    coverBits.resize(size_t(c.stride) * c.h);
    if (fread(coverBits.data(), 1, coverBits.size(), f) == coverBits.size()) {
      b = {coverBits.data(), c.w, c.h, c.stride};
    }
  }
  fclose(f);
  return b;
}

// A square sleep picture (bookprep devpages --picture), kSquareBytes.
std::vector<uint8_t> pictureBits;
ui::Bitmap loadPicture(const char *path) {
  FILE *f = fopen(path, "rb");
  ui::Bitmap b;
  if (!f) {
    perror(path);
    return b;
  }
  pictureBits.resize(kSquareBytes);
  if (fread(pictureBits.data(), 1, pictureBits.size(), f) == pictureBits.size()) {
    b = {pictureBits.data(), kSquareSide, kSquareSide, kSquareStride};
  } else {
    fprintf(stderr, "%s: no %dx%d picture\n", path, kSquareSide, kSquareSide);
  }
  fclose(f);
  return b;
}

void status(const char *chapter, uint32_t page, uint32_t count, int battery,
            StatusBarMode mode = StatusBarMode::Full, bool bookmarked = false, bool locked = false) {
  ui::StatusView v;
  v.chapter = chapter;
  v.page = page;
  v.pageCount = count;
  v.batteryPct = battery;
  v.mode = mode;
  v.bookmarked = bookmarked;
  v.locked = locked;
  ui::statusBar(v);
}

void sleep(const char *title, const char *author, int pct, int battery, ui::Bitmap cover = {},
           bool locked = false, ui::Bitmap picture = {}) {
  ui::SleepView v;
  v.picture = picture;
  v.title = title;
  v.author = author;
  v.percentRead = pct;
  v.batteryPct = battery;
  v.cover = cover;
  v.locked = locked;
  ui::sleepScreen(v);
}

#define DOT "  \xC2\xB7  "
using ui::Button;
const ui::Hint kNext = {Button::Next, ui::kArrowDown, ui::kArrowUp};
const ui::Hint kOpen = {Button::Menu, "open", "back"};
const ui::Hint kGo = {Button::Menu, "go", "back"};

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
  strncpy(it.label, settingLabel(SettingId(i), ui::holdSettingLabel()), sizeof(it.label) - 1);
  settingValue(s, SettingId(i), it.detail, sizeof(it.detail));
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: ui_preview <out_dir> <sample.t5b> <square-picture.bin> [orientation]\n");
    return 2;
  }
  outDir = argv[1];
  const char *sample = argv[2];
  const char *picture = argv[3];
  display::begin();
  // Settings -> Orientation (boards that turn: 2 = landscape on the 7.5")
  setOrientationCount(kScreenCount > 1 ? 4 : 2);
  if (argc > 4) display::setOrientation(uint8_t(atoi(argv[4])));
  ListModel list;
  const char *title = "Max Havelaar / Of de koffiveilingen der Nederlandsche Handelsmaatschappy";

  if (!loadPage(sample, 1)) return 1;
  status("EERSTE HOOFDSTUK", 26, 3596, 72);
  save("01-reading");

  if (!loadPage(sample, 0)) return 1;
  status("Hoofdstuk \xC3\xA9\xC3\xA9n \xE2\x80\x94 De koffiveilingen der Nederlandsche "
                "Handelsmaatschappy",
                2400, 3596, 8);
  save("02-reading-long-title");

  list.reset(7, ui::menuRows(), 0);
  ui::menu(title, list, rowItem, (void *)kMain, kNext, kOpen, 72);
  save("03-main-menu");

  list.reset(10, ui::menuRows(), 2);
  ui::menu("Chapters", list, rowItem, (void *)kChapters, kNext, kGo, 72);
  save("04-chapters");

  ui::valueEditor("Go to position", "37%", "Page 1330 of 3596", 37, {Button::Next, "+1", "+10"}, kGo,
                  72);
  save("05-go-to");

  list.reset(4, ui::menuRows(), 1);
  ui::menu("Library", list, rowItem, (void *)kBooks, kNext, kOpen, 72);
  save("06-library");

  list.reset(kSettingCount, ui::menuRows(), 4);
  ui::menu("Settings", list, settingItem, nullptr, kNext, {Button::Menu, "edit", "back"}, 72);
  save("07-settings");

  const char *labels[] = {"Firmware", "Battery", "Book", "Position", "Memory", "Wake to page"};
  const char *values[] = {"0.7.0", "3.92 V" DOT "72%", title, "page 27/3596" DOT "ch. 3/25",
                          "card 6.2 GB free" DOT "214 KB RAM", "812 ms" DOT "1532 wakes"};
  ui::details("About", labels, values, 6, {Button::None, nullptr, nullptr}, {Button::Menu, "back", nullptr},
              72);
  save("08-about");

  ui::message(ui::Icon::Card, "No SD card", "Insert a FAT32 card", "with books in /books",
              ui::retryHint());
  save("09-no-card");

  ui::message(ui::Icon::Book, "No books yet", "Upload them over Wi-Fi, or copy",
              ".t5b files to /books on the card", {Button::Next, "Wi-Fi transfer", nullptr},
              {Button::None, nullptr, nullptr});
  save("10-no-books");

  ui::message(ui::Icon::Warning, "Cannot open book", "old.t5b", "made for another screen size",
              ui::retryHint());
  save("11-error");

  sleep(title, "Multatuli", 37, 72);
  save("12-sleep");

  sleep("De Kleine Johannes", "Frederik van Eeden", 64, 15);
  save("13-sleep-short-title");

  ui::message(ui::Icon::Battery, "Battery low", "Charge the reader,", "then press a button",
              nullptr);
  save("14-low-battery");

  char join[96];
  wifiQrPayload("T5-Reader-1A2B", "k7m4q9xp", join, sizeof(join));
  ui::wifiScreen({join, 1, "Join the Wi-Fi", "Network", "T5-Reader-1A2B", "Password", "k7m4q9xp", ""},
                 {Button::Next, "show page", nullptr}, {Button::Menu, "done", nullptr}, 72);
  save("15-wifi-join");

  ui::wifiScreen({"http://192.168.4.1/?k=4821", 2, "Open the page", "Address", "192.168.4.1",
                  "Code", "4821", "Received Max Havelaar"},
                 {Button::Next, "show Wi-Fi", nullptr}, {Button::Menu, "done", nullptr}, 72);
  save("16-wifi-page");

  // --- later screens ---
  const Row kMarks[] = {
      {"Remove bookmark here", "", false, false},
      {"EERSTE HOOFDSTUK", "27", false, true},
      {"TWEEDE HOOFDSTUK", "88", false, false},
      {"ZESDE HOOFDSTUK", "570", false, false},
  };
  list.reset(4, ui::menuRows(), 1);
  ui::menu("Bookmarks", list, rowItem, (void *)kMarks, kNext, kGo, 72);
  save("17-bookmarks");

  const char *statLabels[] = {"Today", "Streak", "Days read", "Total"};
  const char *statValues[] = {"23 pages" DOT "31 min", "6 days" DOT "best 14", "41",
                              "3412 pages" DOT "61 h 12 min"};
  ui::details("Statistics", statLabels, statValues, 4, {Button::None, nullptr, nullptr},
              {Button::Menu, "back", nullptr}, 72);
  save("18-statistics");

  if (!loadPage(sample, 1)) return 1;
  status("EERSTE HOOFDSTUK", 26, 3596, 72, StatusBarMode::Full, true, true);
  save("19-reading-locked-bookmarked");

  if (!loadPage(sample, 1)) return 1;
  status("EERSTE HOOFDSTUK", 26, 3596, 72, StatusBarMode::Minimal, true);
  save("20-reading-minimal");

  sleep(title, "Multatuli", 37, 72, loadCover(sample), true);
  save("21-sleep-cover-locked");

  const ui::Bitmap pic = loadPicture(picture);
  if (!pic.bits) return 1;
  sleep(title, "Multatuli", 37, 72, {}, false, pic);
  save("25-sleep-picture-book");
  sleep("De Kleine Johannes", "Frederik van Eeden", 64, 15, {}, true, pic);
  save("26-sleep-picture-short-locked");
  sleep(nullptr, nullptr, -1, 72, {}, false, pic);
  save("27-sleep-picture-no-book");

  const Row kShelf[] = {
      {"Engels", "4 books", true, false},
      {"Klassiekers", "12 books", true, true},
      {"De Kleine Johannes", "64%", false, false},
      {"Een Nederlandsch \xE2\x80\x9C" "ABC\xE2\x80\x9D", "new", false, false},
  };
  list.reset(4, ui::menuRows(), 1);
  ui::menu("Davids reader", list, rowItem, (void *)kShelf, kNext, kOpen, 72);
  save("22-library-folders");

  // --- book server (app.cpp: Menu -> Book server) ---
  const bool wide = display::screen().w >= 700;  // the author beside the title (app.cpp)
  const Row kServerTop[] = {
      {tr(S::Newest), "", true, false},
      {tr(S::Authors), "", true, false},
      {tr(S::Titles), "", true, false},
  };
  list.reset(3, ui::menuRows(), 0);
  ui::menu(tr(S::BookServer), list, rowItem, (void *)kServerTop, kNext, kOpen, 72);
  save("28-server-top");
  const Row kServerBooks[] = {
      {"Het geheime leven van schrijvers", wide ? "Guillaume Musso" : "", false, false},
      {"Het boek van de Baltimores", wide ? "Jo\xC3\xABl Dicker" : "", false, true},
      {"Dieren eten", wide ? "Jonathan Safran Foer" : "", false, false},
      {"Gids voor de kantoorjungle", wide ? "Japke-d. Bouma" : "", false, false},
      {"Je hebt w\xC3\xA9l iets te verbergen", wide ? "Maurits Martijn, Dimitri To\xE2\x80\xA6" : "",
       false, false},
      {"Mrs Dalloway", wide ? "Virginia Woolf" : "", false, true},
  };
  list.reset(6, ui::menuRows(), 2);
  ui::menu(tr(S::Newest), list, rowItem, (void *)kServerBooks, kNext, kOpen, 72);
  save("29-server-newest");
  ui::valueEditor(tr(S::Typesetting), "42%", "Dieren eten", 42, {Button::None, nullptr, nullptr},
                  {Button::Menu, "back", nullptr}, 72);
  save("30-server-typesetting");
  ui::valueEditor(tr(S::Downloading), "60%", "1.2 MB of 1.9 MB", 60,
                  {Button::None, nullptr, nullptr}, {Button::None, nullptr, nullptr}, 72);
  save("31-server-downloading");
  ui::message(ui::Icon::Book, tr(S::Downloaded), "Dieren eten (1\xE2\x80\x93" "151)",
              "Jonathan Safran Foer", {Button::Next, tr(S::ReadAction), nullptr},
              {Button::Menu, tr(S::MoreAction), nullptr});
  save("32-server-downloaded");

  setLanguage(Lang::Nl);
  list.reset(kSettingCount, ui::menuRows(), 0);
  ui::menu(tr(S::Settings), list, settingItem, nullptr, kNext,
           {Button::Menu, tr(S::Edit), tr(S::Back)}, 72);
  save("23-settings-nl");
  ui::message(ui::Icon::Book, tr(S::NoBooksYet), tr(S::NoBooksLine1), tr(S::NoBooksLine2),
              {Button::Next, tr(S::WifiTransfer), nullptr}, {Button::None, nullptr, nullptr});
  save("24-no-books-nl");
#ifdef BOARD_FIREBEETLE_75
  // the same without a card (reader_firebeetle75_flash, app.cpp showNoBooks())
  char room[48];
  snprintf(room, sizeof(room), tr(S::NoBooksFlash2), 170u);
  ui::message(ui::Icon::Book, tr(S::NoBooksYet), tr(S::NoBooksFlash1), room,
              {Button::Next, tr(S::WifiTransfer), nullptr}, {Button::None, nullptr, nullptr});
  save("25-no-books-flash-nl");
#endif
  // book server screens in Dutch (the longest texts)
  char part[3][48];
  snprintf(part[0], sizeof(part[0]), tr(S::PagesRange), 1u, 151u);
  snprintf(part[1], sizeof(part[1]), tr(S::PagesRange), 152u, 297u);
  snprintf(part[2], sizeof(part[2]), tr(S::PagesRange), 298u, 468u);
  const Row kParts[] = {
      {part[0], "1.9 MB", false, true},
      {part[1], "1.9 MB", false, false},
      {part[2], "2.1 MB", false, false},
  };
  list.reset(3, ui::menuRows(), 1);
  ui::menu(tr(S::ChooseAPart), list, rowItem, (void *)kParts, kNext,
           {Button::Menu, tr(S::Open), tr(S::Back)}, 72);
  save("33-server-parts-nl");
  ui::message(ui::Icon::Warning, tr(S::NoHomeWifi), tr(S::SetOnPage1), tr(S::SetOnPage2),
              tr(S::BackAnyButton));
  save("34-server-no-wifi-nl");
  ui::message(ui::Icon::Warning, tr(S::ServerUnreachable), "192.168.68.101:8484", tr(S::NoAnswer),
              tr(S::BackAnyButton));
  save("35-server-unreachable-nl");
  char waiting[48];
  snprintf(waiting, sizeof(waiting), tr(S::AheadInLine), 2u);
  ui::valueEditor(tr(S::Typesetting), "0%", waiting, 0, {Button::None, nullptr, nullptr},
                  {Button::Menu, tr(S::Back), nullptr}, 72);
  save("36-server-waiting-nl");
  ui::message(ui::Icon::Book, tr(S::NoBooksYet), tr(S::NoBooksLine1), tr(S::NoBooksLine2),
              {Button::Next, tr(S::WifiTransfer), nullptr}, {Button::Menu, tr(S::BookServer), nullptr});
  save("37-no-books-server-nl");
  setLanguage(Lang::En);
  return 0;
}
