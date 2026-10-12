// The reader: wake handling, reading screen, menus, sleep.
//
// With "Sleep after page" on (default) every page turn is one wake-up: setup()
// classifies the button that woke the chip, shows the next/previous page and
// goes back to deep sleep with an idle timer. When that timer fires instead of
// a button, the sleep screen is shown and only a button wakes the reader.
// Menus run awake in loop() until a book page is shown again or the idle
// timeout hits.
#include "app.h"

#include <Arduino.h>
#include <esp_attr.h>
#include <string.h>

#include <vector>

#include "board.h"
#include "book.h"
#include "display.h"
#include "input.h"
#include "menu.h"
#include "policy.h"
#include "power.h"
#include "prefs.h"
#include "settings.h"
#include "storage.h"
#include "text_util.h"
#include "ui.h"

namespace app {
namespace {

using input::Key;

enum class Screen : uint8_t {
  None,
  Reading,
  Sleeping,
  LowBattery,
  Message,
  MainMenu,
  Chapters,
  GoTo,
  Library,
  SettingsMenu,
  About,
};

// What the panel shows and where we are, kept through deep sleep (RTC RAM).
constexpr uint32_t kRtcMagic = 0x54354231;
struct RtcState {
  uint32_t magic;
  char bookPath[128];
  uint32_t page;
  int32_t chapter;
  Screen screen;
  uint32_t wakes;
  uint32_t imageMs;  // app start -> image refreshed, last time
};
RTC_DATA_ATTR RtcState rtc;

constexpr uint32_t kNoBatteryMv = 2500;  // below this the ADC sees no battery (USB only)
const char *const kListHint = "NEXT:down hold:up  MENU:ok hold:back";

Settings settings;
Book book;
uint32_t page = 0;
uint32_t batteryMv = 0;
int batteryPct = -1;
Screen screen = Screen::None;
ListModel list;
std::vector<storage::BookEntry> library;
int goToPct = 0;

const char *const kMainItems[] = {"Resume", "Chapters", "Go to %", "Library",
                                  "Settings", "About", "Sleep"};
enum MainItem { kResume, kChapters, kGoTo, kLibrary, kSettings, kAbout, kSleep, kMainCount };

void setScreen(Screen s) {
  screen = s;
  rtc.screen = s;
}

// --- sleep -------------------------------------------------------------------

[[noreturn]] void enterSleep(bool withSleepScreen) {
  if (withSleepScreen) {
    ui::sleepScreen(book.isOpen() ? book.header().title : nullptr,
                    book.isOpen() ? book.header().author : nullptr, batteryPct);
    display::show(Refresh::Full);
    setScreen(Screen::Sleeping);
  }
  const bool reading = rtc.screen == Screen::Reading;
  display::sleep(reading && settings.wakeRefresh == WakeRefresh::Retain);
  input::waitAllReleased();
  // The idle timer only matters while a page is shown: it brings up the sleep screen.
  power::deepSleep(reading ? uint32_t(settings.idleMinutes) * 60 : 0);
}

void showMessage(const char *title, const char *line1, const char *line2) {
  ui::message(title, line1, line2, "any button: try again");
  display::show(Refresh::Full);
  setScreen(Screen::Message);
  Serial.printf("message: %s / %s / %s\n", title, line1 ? line1 : "", line2 ? line2 : "");
}

// --- reading -----------------------------------------------------------------

const char *baseName(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

// quiet: no error screen (start-up falls back to the library instead)
bool openBook(const char *path, bool quiet = false) {
  if (!book.open(path)) {
    Serial.printf("cannot open %s: %s\n", path, book.error());
    if (!quiet) showMessage("Cannot open book", baseName(path), book.error());
    rtc.bookPath[0] = '\0';
    return false;
  }
  strlcpy(rtc.bookPath, path, sizeof(rtc.bookPath));
  prefs::setLastBook(path);
  page = prefs::bookmark(book.id());
  if (page >= book.pageCount()) page = book.pageCount() - 1;
  rtc.chapter = -1;
  return true;
}

void showReading(Refresh requested) {
  if (!book.readPage(page, display::framebuffer())) {
    showMessage("Read error", baseName(book.path()), "card removed?");
    return;
  }
  const int chapter = book.chapterForPage(page);
  t5b::TocEntry entry;
  const char *title = chapter >= 0 && book.tocEntry(chapter, entry) ? entry.title : book.header().title;
  ui::statusBar(title, page, book.pageCount(), batteryPct);

  Refresh mode = Refresh::Full;
  if (requested == Refresh::Partial) {
    const bool chapterChanged = rtc.screen == Screen::Reading && chapter != rtc.chapter;
    mode = decideRefresh(true, chapterChanged, display::shownValid(), display::partialsSinceFull(),
                         settings.fullEvery);
    if (mode == Refresh::Partial && settings.deepSleep && settings.wakeRefresh == WakeRefresh::Full) {
      mode = Refresh::Fast;
    }
  }
  const Refresh used = display::show(mode);
  setScreen(Screen::Reading);
  rtc.page = page;
  rtc.chapter = chapter;
  prefs::setBookmark(book.id(), page);
  rtc.imageMs = millis();
  const display::Timing t = display::lastTiming();
  Serial.printf("page %u/%u chapter %d: %s refresh, data %u ms, refresh %u ms, at %u ms\n",
                unsigned(page + 1), unsigned(book.pageCount()), chapter,
                used == Refresh::Full ? "full" : used == Refresh::Fast ? "fast" : "partial",
                unsigned(t.dataMs), unsigned(t.refreshMs), unsigned(rtc.imageMs));
}

// After a page is on the panel: sleep right away, or stay awake (setting).
void pageShown() {
  input::setRepeat(false);
  if (screen == Screen::Reading && settings.deepSleep) enterSleep(false);
}

void resume() {
  showReading(Refresh::Full);  // full: clears the menu's ghosting
  pageShown();
}

// --- menus -------------------------------------------------------------------

Refresh menuRefresh() {
  return decideRefresh(true, false, display::shownValid(), display::partialsSinceFull(),
                       settings.fullEvery);
}

void mainItemText(int i, char *out, size_t n, void *) { strlcpy(out, kMainItems[i], n); }

void renderMainMenu() {
  ui::menu(book.header().title, list, mainItemText, nullptr, kListHint, batteryPct);
  display::show(menuRefresh());
}

void openMainMenu() {
  input::setRepeat(true);
  list.reset(kMainCount, ui::kMenuRows, kResume);
  setScreen(Screen::MainMenu);
  renderMainMenu();
}

void chapterText(int i, char *out, size_t n, void *) {
  t5b::TocEntry e;
  if (!book.tocEntry(i, e)) {
    strlcpy(out, "?", n);
    return;
  }
  const bool current = i == book.chapterForPage(page);
  snprintf(out, n, "%s%s", current ? "\xE2\x80\xA2 " : "", e.title);
}

void renderChapters() {
  char hint[48] = "no chapters";
  if (list.count()) {
    snprintf(hint, sizeof(hint), "p%u  NEXT:down hold:up MENU:go",
             unsigned(book.chapterStart(list.selected()) + 1));
  }
  ui::menu("Chapters", list, chapterText, nullptr, hint, batteryPct);
  display::show(menuRefresh());
}

void openChapters() {
  const int current = book.chapterForPage(page);
  list.reset(book.tocCount(), ui::kMenuRows, current < 0 ? 0 : current);
  setScreen(Screen::Chapters);
  renderChapters();
}

uint32_t pctToPage(int pct) { return uint32_t(uint64_t(book.pageCount() - 1) * pct / 100); }

void renderGoTo() {
  char value[32];
  snprintf(value, sizeof(value), "%d%%  (p %u)", goToPct, unsigned(pctToPage(goToPct) + 1));
  ui::valueEditor("Go to", value, goToPct, "NEXT:+1 hold:+10  MENU:go hold:back");
  display::show(menuRefresh());
}

void openGoTo() {
  goToPct = book.pageCount() > 1 ? int(uint64_t(page) * 100 / (book.pageCount() - 1)) : 0;
  setScreen(Screen::GoTo);
  renderGoTo();
}

void libraryText(int i, char *out, size_t n, void *) {
  const bool current = book.isOpen() && library[i].path == book.path();
  snprintf(out, n, "%s%s", current ? "\xE2\x80\xA2 " : "", library[i].title.c_str());
}

void renderLibrary() {
  String hint = kListHint;
  if (list.count()) {
    const storage::BookEntry &e = library[list.selected()];
    hint = e.author.length() ? e.author : String("(no author)");
    hint += "  " + String(e.pages) + "p";
  }
  ui::menu("Library", list, libraryText, nullptr, hint.c_str(), batteryPct);
  display::show(menuRefresh());
}

void openLibrary() {
  const storage::Status status = storage::listBooks(library);
  if (status != storage::Status::Ok) {
    showMessage("No books", "Copy .t5b files to /books", "on the SD card");
    return;
  }
  int selected = 0;
  for (size_t i = 0; i < library.size(); i++) {
    if (book.isOpen() && library[i].path == book.path()) selected = int(i);
  }
  input::setRepeat(true);
  list.reset(int(library.size()), ui::kMenuRows, selected);
  setScreen(Screen::Library);
  renderLibrary();
}

void settingText(int i, char *out, size_t n, void *) { describeSetting(settings, SettingId(i), out, n); }

void renderSettings() {
  ui::menu("Settings", list, settingText, nullptr, "NEXT:down hold:up  MENU:change hold:back",
           batteryPct);
  display::show(menuRefresh());
}

void openSettings() {
  list.reset(kSettingCount, ui::kMenuRows, 0);
  setScreen(Screen::SettingsMenu);
  renderSettings();
}

void openAbout() {
  char l[6][80];
  snprintf(l[0], sizeof(l[0]), "Firmware %s, bb_epaper 2.1.11+", FW_VERSION);
  if (batteryPct >= 0) {
    snprintf(l[1], sizeof(l[1]), "Battery %u.%02u V (%d%%)", unsigned(batteryMv / 1000),
             unsigned(batteryMv % 1000 / 10), batteryPct);
  } else {
    snprintf(l[1], sizeof(l[1]), "Battery: none (USB power)");
  }
  snprintf(l[2], sizeof(l[2]), "%s", book.header().title);
  snprintf(l[3], sizeof(l[3]), "Page %u of %u, chapter %d of %d", unsigned(page + 1),
           unsigned(book.pageCount()), book.chapterForPage(page) + 1, book.tocCount());
  snprintf(l[4], sizeof(l[4]), "SD %u MB, free heap %u KB", unsigned(storage::cardSizeMb()),
           unsigned(ESP.getFreeHeap() / 1024));
  snprintf(l[5], sizeof(l[5]), "Wake to image %u ms, wakes %u", unsigned(rtc.imageMs),
           unsigned(rtc.wakes));
  const char *lines[] = {l[0], l[1], l[2], l[3], l[4], l[5]};
  ui::textScreen("About", lines, 6, "any button: back");
  display::show(menuRefresh());
  setScreen(Screen::About);
}

// --- key handling --------------------------------------------------------------

enum class ListAction { None, Moved, Select, Back };

ListAction listKey(Key k) {
  switch (k) {
    case Key::NextShort: list.next(); return ListAction::Moved;
    case Key::NextLong:
    case Key::NextRepeat: list.prev(); return ListAction::Moved;
    case Key::MenuShort: return ListAction::Select;
    case Key::MenuLong: return ListAction::Back;
    case Key::None: break;
  }
  return ListAction::None;
}

void readingKey(Key k) {
  switch (k) {
    case Key::NextShort:
      if (page + 1 < book.pageCount()) {
        page++;
        showReading(Refresh::Partial);
      }
      pageShown();
      break;
    case Key::NextLong:
      if (page > 0) {
        page--;
        showReading(Refresh::Partial);
      }
      pageShown();
      break;
    case Key::MenuShort: openMainMenu(); break;
    case Key::MenuLong: enterSleep(true);
    default: break;
  }
}

void mainMenuKey(Key k) {
  switch (listKey(k)) {
    case ListAction::Moved: renderMainMenu(); break;
    case ListAction::Back: resume(); break;
    case ListAction::Select:
      switch (list.selected()) {
        case kResume: resume(); break;
        case kChapters: openChapters(); break;
        case kGoTo: openGoTo(); break;
        case kLibrary: openLibrary(); break;
        case kSettings: openSettings(); break;
        case kAbout: openAbout(); break;
        case kSleep: enterSleep(true);
      }
      break;
    case ListAction::None: break;
  }
}

void chaptersKey(Key k) {
  switch (listKey(k)) {
    case ListAction::Moved: renderChapters(); break;
    case ListAction::Back: openMainMenu(); break;
    case ListAction::Select:
      if (list.count()) {
        page = book.chapterStart(list.selected());
        resume();
      }
      break;
    case ListAction::None: break;
  }
}

void goToKey(Key k) {
  switch (k) {
    case Key::NextShort:
      goToPct = goToPct >= 100 ? 0 : goToPct + 1;
      renderGoTo();
      break;
    case Key::NextLong:
    case Key::NextRepeat:
      goToPct = goToPct >= 100 ? 0 : (goToPct + 10 > 100 ? 100 : goToPct + 10);
      renderGoTo();
      break;
    case Key::MenuShort:
      page = pctToPage(goToPct);
      resume();
      break;
    case Key::MenuLong: openMainMenu(); break;
    case Key::None: break;
  }
}

void libraryKey(Key k) {
  switch (listKey(k)) {
    case ListAction::Moved: renderLibrary(); break;
    case ListAction::Back:
      if (book.isOpen()) {
        openMainMenu();
      } else {
        renderLibrary();  // nothing to go back to
      }
      break;
    case ListAction::Select: {
      const String path = library[list.selected()].path;
      if (book.isOpen() && path == book.path()) {
        resume();
      } else if (openBook(path.c_str())) {
        resume();
      }
      break;
    }
    case ListAction::None: break;
  }
}

void settingsKey(Key k) {
  switch (listKey(k)) {
    case ListAction::Moved: renderSettings(); break;
    case ListAction::Back: openMainMenu(); break;
    case ListAction::Select:
      cycleSetting(settings, SettingId(list.selected()));
      prefs::saveSettings(settings);
      input::setLongPress(settings.longPressMs);
      renderSettings();
      break;
    case ListAction::None: break;
  }
}

void handle(Key k) {
  if (k == Key::None) return;
  switch (screen) {
    case Screen::Reading: readingKey(k); break;
    case Screen::MainMenu: mainMenuKey(k); break;
    case Screen::Chapters: chaptersKey(k); break;
    case Screen::GoTo: goToKey(k); break;
    case Screen::Library: libraryKey(k); break;
    case Screen::SettingsMenu: settingsKey(k); break;
    case Screen::About: openMainMenu(); break;
    case Screen::Message:
      ESP.restart();  // retry from the start (card inserted, book copied)
      break;
    case Screen::Sleeping:
    case Screen::LowBattery:
    case Screen::None:
      if (book.isOpen()) resume();
      break;
  }
}

// --- start-up ----------------------------------------------------------------

// Opens the book from RTC (fast path, survives deep sleep) or NVS (last book).
bool restoreBook() {
  if (rtc.bookPath[0]) {
    const uint32_t rtcPage = rtc.page;
    const int32_t rtcChapter = rtc.chapter;
    if (openBook(rtc.bookPath, true)) {
      page = rtcPage < book.pageCount() ? rtcPage : book.pageCount() - 1;
      rtc.chapter = rtcChapter;
      return true;
    }
  }
  const String last = prefs::lastBook();
  if (last.length() && openBook(last.c_str(), true)) return true;
  return false;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  const power::Wake wake = power::wakeReason();
  prefs::begin();
  settings = prefs::loadSettings();
  input::begin(settings.longPressMs);
  Key key = Key::None;
  if (wake == power::Wake::NextButton) key = input::wakePress(kBtnNext);
  if (wake == power::Wake::MenuButton) key = input::wakePress(kBtnMenu);

  if (rtc.magic != kRtcMagic) {
    memset(&rtc, 0, sizeof(rtc));
    rtc.magic = kRtcMagic;
    rtc.chapter = -1;
  }
  rtc.wakes++;
  const Screen shown = rtc.screen;
  batteryMv = power::batteryMv();
  batteryPct = batteryMv > kNoBatteryMv ? batteryPercent(batteryMv) : -1;
  Serial.printf("\nT5 reader %s: wake by %s, key %d, battery %u mV, shown screen %d\n", FW_VERSION,
                power::wakeName(wake), int(key), unsigned(batteryMv), int(shown));

  display::begin();
  if (batteryMv > kNoBatteryMv && batteryMv < kBatteryLowMv) {
    if (shown != Screen::LowBattery) {
      ui::message("Battery low", "Please charge the reader", nullptr, "");
      display::show(Refresh::Full);
      setScreen(Screen::LowBattery);
    }
    display::sleep(false);
    power::deepSleep(0);
  }
  if (!storage::mount()) {
    showMessage("No SD card", "Insert a FAT32 card", "with books in /books");
    return;
  }
  if (!restoreBook()) {
    openLibrary();  // first start, or the last book is gone
    return;
  }

  if (wake == power::Wake::Timer) enterSleep(true);  // idle while a page was shown
  if (shown != Screen::Reading || key == Key::None) {
    // power-on, or waking from the sleep/low-battery/message screen: no page turn
    resume();
    return;
  }
  readingKey(key);
}

void loop() {
  handle(input::poll());
  const uint32_t idleMs = uint32_t(settings.idleMinutes) * 60000;
  if (millis() - input::lastActivityMs() > idleMs) enterSleep(screen != Screen::Message);
  delay(5);
}

}  // namespace app
