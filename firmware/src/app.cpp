// The reader: wake handling, reading screen, menus, sleep.
//
// With "Sleep after page" on (default) every page turn is one wake-up: setup()
// classifies the button that woke the chip, shows the next/previous page and
// goes back to deep sleep with an idle timer. When that timer fires instead of
// a button, the sleep screen is shown and only a button wakes the reader.
// Menus run awake in loop() until a book page is shown again or the idle
// timeout hits. Boards that nap between pages (kNapBetweenPages: M5, 7.5") stay
// in loop() and light-sleep there instead; the 7.5" goes to deep sleep when idle.
#include "app.h"

#include <Arduino.h>
#include <esp_attr.h>
#include <string.h>
#include <time.h>

#include <type_traits>
#include <vector>

#include "board.h"
#include "book.h"
#include "bookserver.h"
#include "bookmarks.h"
#include "catalog.h"
#include "display.h"
#include "i18n.h"
#include "input.h"
#include "menu.h"
#include "policy.h"
#include "power.h"
#include "prefs.h"
#include "rtc_clock.h"
#include "reading_stats.h"
#include "settings.h"
#include "storage.h"
#include "text_util.h"
#include "touch.h"
#include "transfer.h"
#include "ui.h"
#include "web_util.h"

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
  Bookmarks,
  GoTo,
  Library,
  Statistics,
  SettingsMenu,
  About,
  WifiTransfer,
  ServerList,  // book server: profiles, Newest/Authors/Titles, books, parts
  ServerBusy,  // book server: typesetting on the server (downloads block)
  ServerDone,  // book server: downloaded, read it or get more
};

// What the panel shows and where we are, kept through deep sleep (RTC RAM).
constexpr uint32_t kRtcMagic = 0x54354233;
struct RtcState {
  uint32_t magic;
  char bookPath[128];
  uint32_t page;
  int32_t chapter;
  Screen screen;
  int8_t pageBattery;  // battery % in the status bar of the page shown
  uint32_t wakes;
  uint32_t imageMs;    // app start -> image refreshed, last time
  ReadingStats stats;  // flushed to NVS every kStatsFlushPages pages and before sleep
  uint16_t statsUnsaved;
};
RTC_DATA_ATTR RtcState rtc;
// A constructor would run on every wake and reset what deep sleep kept (it did:
// default values in ReadingStats zeroed the statistics at every page turn).
static_assert(std::is_trivially_default_constructible<RtcState>::value,
              "RTC_DATA_ATTR state must not have a constructor");
constexpr uint16_t kStatsFlushPages = 20;

constexpr uint32_t kNoBatteryMv = 2500;  // below this the ADC sees no battery (USB only)
using ui::Button;

Settings settings;
Book book;
PageList marks;  // bookmarks of the open book
uint32_t page = 0;
uint32_t batteryMv = 0;
int batteryPct = -1;
bool locked = false;
Screen screen = Screen::None;
ListModel list;
std::vector<storage::BookEntry> library;
String libraryFolder;  // "" = /books
int goToPct = 0;

const S kMainItems[] = {S::Resume,     S::Chapters,   S::Bookmarks,    S::GoTo,
                        S::Library,    S::BookServer, S::Statistics,   S::WifiTransfer,
                        S::Settings,   S::About,      S::Lock,         S::Sleep};
constexpr int kMainCount = sizeof(kMainItems) / sizeof(kMainItems[0]);
// Without an open book (none yet, or it can't be read): what doesn't need one, so
// Wi-Fi transfer and Settings stay reachable from the library.
const S kHomeItems[] = {S::Library,  S::BookServer, S::Statistics, S::WifiTransfer,
                        S::Settings, S::About,      S::Sleep};
constexpr int kHomeCount = sizeof(kHomeItems) / sizeof(kHomeItems[0]);
const S *mainItems() { return book.isOpen() ? kMainItems : kHomeItems; }
int mainCount() { return book.isOpen() ? kMainCount : kHomeCount; }
// What a key does on a message screen.
enum class AfterMessage : uint8_t {
  Restart,      // start again (card inserted, book copied)
  NoBooks,      // "No books yet": Wi-Fi transfer, or MENU the book server if there is one
  Library,      // "Made for landscape", read errors: back to the library
  ServerList,   // a book the server couldn't make or the reader can't hold: back to the list
  LeaveServer,  // no Wi-Fi, no server: out of the book server
};
AfterMessage afterMessage = AfterMessage::Restart;
// A key pressed while the panel refreshed (display::setIdleHook): loop() takes it
// before it reads the buttons, and a page isn't slept on while one waits.
Key pendingKey = Key::None;
// When the page turn in progress began, for "Wake to page" in About: the start of the
// app (a wake from deep sleep), the wake from a nap, or the key (awake).
uint32_t turnStartMs = 0;
bool wokeFromNap = false;
int wifiStep = 0;                // QR shown: 0 = automatic, 1 = join Wi-Fi, 2 = open page

// Footer hints: button role, short-press action, long-press action.
ui::Hint listNext() { return {Button::Next, ui::kArrowDown, ui::kArrowUp}; }
ui::Hint menuHint(S action) { return {Button::Menu, tr(action), tr(S::Back)}; }
ui::Hint backHint() { return {Button::Menu, tr(S::Back), nullptr}; }
const ui::Hint kNoHint = {Button::None, nullptr, nullptr};

Settings boardDefaults() {
  Settings d;
  if (kHasTouch) d.holdAction = HoldAction::Menu;  // long press opens the menu
  return d;
}

void applySettings() {
  setLanguage(settings.language);
  input::setLongPress(settings.longPressMs);
  input::setFlipped(settings.orientation & 1);
  touch::setFlipped(settings.orientation & 1);
  display::setOrientation(settings.orientation);  // a change makes the next refresh full
}

void setScreen(Screen s) {
  screen = s;
  rtc.screen = s;
}

// The display's idle hook. Repeats of a held button are left out: only a press
// counts, and the reading screen has nothing to do with a repeat.
void pollWhileRefreshing() {
  const Key k = input::poll();
  if (k != Key::None && k != Key::NextRepeat && pendingKey == Key::None) pendingKey = k;
}

uint32_t clockNow() { return uint32_t(time(nullptr)); }

void flushStats() {
  if (!rtc.statsUnsaved) return;
  prefs::saveStats(rtc.stats);
  rtc.statsUnsaved = 0;
}

void setLocked(bool on) {
  locked = on;
  prefs::setLocked(on);
}

// --- sleep -------------------------------------------------------------------

int percentRead() {
  if (!book.isOpen()) return -1;
  return book.pageCount() > 1 ? int(uint64_t(page) * 100 / (book.pageCount() - 1)) : 100;
}

void showReading(Refresh requested);

// The next sleep picture of a shape (in turn) into `out`; false: none.
bool readNextPicture(storage::PictureShape shape, uint8_t *out, size_t size) {
  const int n = storage::pictureCount(shape);
  if (n <= 0) return false;
  const uint32_t i = prefs::nextPicture() % uint32_t(n);
  fs::File f = storage::files().open(storage::picturePath(shape, int(i)), FILE_READ);
  const bool ok = f && f.read(out, size) == size;
  if (f) f.close();
  if (ok) prefs::setNextPicture(i + 1);
  return ok;
}

// The sleep screen of the "Sleep screen" setting into the framebuffer: a picture
// from /sleep, or title, author and progress (with the cover or a square picture
// if asked for).
void drawSleepScreen() {
  if (settings.sleepScreen == SleepScreen::Pictures &&
      readNextPicture(storage::PictureShape::Full, display::framebuffer(), kPageBytes)) {
    if (locked) ui::lockBadge();
    return;
  }
  ui::SleepView v;
  v.title = book.isOpen() ? book.header().title : nullptr;
  v.author = book.isOpen() ? book.header().author : nullptr;
  v.percentRead = percentRead();
  v.batteryPct = batteryPct;
  v.locked = locked;
  uint8_t *coverBits = nullptr;
  if (settings.sleepScreen == SleepScreen::Cover && book.isOpen() && book.cover().present) {
    const t5b::Cover &c = book.cover();
    coverBits = static_cast<uint8_t *>(malloc(size_t(c.stride) * c.h));
    if (coverBits && book.readCover(coverBits)) v.cover = {coverBits, c.w, c.h, c.stride};
  }
  uint8_t *pictureBits = nullptr;
  if (settings.sleepScreen == SleepScreen::PictureBook) {
    pictureBits = static_cast<uint8_t *>(malloc(kSquareBytes));
    if (pictureBits &&
        readNextPicture(storage::PictureShape::Square, pictureBits, size_t(kSquareBytes))) {
      v.picture = {pictureBits, kSquareSide, kSquareSide, kSquareStride};
    }
  }
  ui::sleepScreen(v);
  free(pictureBits);
  free(coverBits);
}

[[noreturn]] void enterSleep(bool withSleepScreen) {
  transfer::stop();
  bookserver::disconnect();
  flushStats();
  uint32_t timer = 0;
  if (withSleepScreen) {
    if (settings.lockOnSleep && book.isOpen()) setLocked(true);
    if (settings.sleepScreen == SleepScreen::Page && book.isOpen()) {
      // Keep the page: the next press just turns it. Drawn again if a menu was
      // up or the lock indicator has to show.
      if (rtc.screen != Screen::Reading || locked) showReading(Refresh::Full);
      setScreen(Screen::Reading);
    } else {
      drawSleepScreen();
      display::show(Refresh::Full);
      setScreen(Screen::Sleeping);
    }
  } else if (rtc.screen == Screen::Reading) {
    timer = uint32_t(settings.idleMinutes) * 60;  // the idle timer brings up the sleep screen
  }
  const bool reading = rtc.screen == Screen::Reading;
  display::sleep(reading && settings.wakeRefresh == WakeRefresh::Retain);
  input::waitAllReleased();
  power::deepSleep(timer);
}

// Asleep again without touching the screen (locked: a press that doesn't unlock).
[[noreturn]] void sleepAgain() {
  display::sleep(rtc.screen == Screen::Reading && settings.wakeRefresh == WakeRefresh::Retain);
  input::waitAllReleased();
  power::deepSleep(0);
}

void showMessage(ui::Icon icon, const char *title, const char *line1, const char *line2) {
  afterMessage = AfterMessage::Restart;
  ui::message(icon, title, line1, line2, ui::retryHint());
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
    if (!quiet) showMessage(ui::Icon::Warning, tr(S::CannotOpenBook), baseName(path), book.error());
    rtc.bookPath[0] = '\0';
    return false;
  }
  if (book.screen() != display::screenIndex()) {  // made for the other orientation (7.5")
    Serial.printf("cannot open %s: made for %s\n", path, kScreens[book.screen()].id);
    book.close();
    rtc.bookPath[0] = '\0';
    return false;
  }
  strlcpy(rtc.bookPath, path, sizeof(rtc.bookPath));
  prefs::setLastBook(path);
  page = prefs::bookmark(book.id());
  if (page >= book.pageCount()) page = book.pageCount() - 1;
  storage::readMarks(book.stem(), book.pageCount(), marks);
  rtc.chapter = -1;
  return true;
}

const char *chapterTitle(uint32_t n, t5b::TocEntry &entry) {
  const int chapter = book.chapterForPage(n);
  return chapter >= 0 && book.tocEntry(chapter, entry) ? entry.title : book.header().title;
}

// Book page n with its status bar into the framebuffer.
bool drawPage(uint32_t n, int battery) {
  if (!book.readPage(n, display::framebuffer())) return false;
  t5b::TocEntry entry;
  ui::StatusView v;
  v.chapter = chapterTitle(n, entry);
  v.page = n;
  v.pageCount = book.pageCount();
  v.batteryPct = battery;
  v.mode = settings.statusBar;
  v.bookmarked = marks.contains(n);
  v.locked = locked;
  ui::statusBar(v);
  return true;
}

// A page that can't be read: say why, and let any button go back to the library.
// The book is forgotten as the last one, or a restart (or a wake) would open it
// and fail again.
void showReadError() {
  const t5b::Error err = book.readError();
  const char *why = err == t5b::Error::TooShort || err == t5b::Error::None
                        ? tr(kBooksInFlash ? S::FileDamaged : S::CardRemoved)
                        : t5b::errorText(err);  // the file's contents, e.g. "corrupt page"
  Serial.printf("message: %s / %s / %s\n", tr(S::ReadError), book.path(), why);
  ui::message(ui::Icon::Card, tr(S::ReadError), baseName(book.path()), why,
              tr(S::BackAnyButton));
  display::show(Refresh::Full);
  setScreen(Screen::Message);
  afterMessage = AfterMessage::Library;
  book.close();
  rtc.bookPath[0] = '\0';
  prefs::setLastBook("");
}

void showReading(Refresh requested) {
  // Big panels (T5-4.7") can't keep the shown image through deep sleep: draw
  // the page that is on the glass again, so this turn can still be partial.
  if (requested == Refresh::Partial && settings.wakeRefresh != WakeRefresh::Full &&
      !display::shownValid() && rtc.screen == Screen::Reading && rtc.page != page &&
      rtc.page < book.pageCount() && drawPage(rtc.page, rtc.pageBattery)) {
    display::assumeShown();
  }
  if (!drawPage(page, batteryPct)) {
    showReadError();
    return;
  }
  const int chapter = book.chapterForPage(page);

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
  rtc.pageBattery = int8_t(batteryPct);
  prefs::setBookmark(book.id(), page);
  rtc.imageMs = millis() - turnStartMs;
  const display::Timing t = display::lastTiming();
  Serial.printf("page %u/%u chapter %d: %s refresh, before it %u ms, refresh %u ms, after %u ms\n",
                unsigned(page + 1), unsigned(book.pageCount()), chapter,
                used == Refresh::Full ? "full" : used == Refresh::Fast ? "fast" : "partial",
                unsigned(t.dataMs), unsigned(t.refreshMs), unsigned(rtc.imageMs));
}

void readBattery() {
  batteryMv = power::batteryMv();
  batteryPct = batteryMv > kNoBatteryMv ? batteryPercent(batteryMv) : -1;
}

void showLowBattery() {
  ui::message(ui::Icon::Battery, tr(S::BatteryLow), tr(S::BatteryLine1), tr(S::BatteryLine2),
              nullptr);
  display::show(Refresh::Full);
  setScreen(Screen::LowBattery);
}

// Boards that nap between pages (kNapBetweenPages: M5, 7.5"): light sleep until a
// touch or button. The idle timer ends in the sleep screen and power-off (M5) or deep
// sleep. Back in loop(), the touch or press that woke the chip is read as usual and
// the panel still holds its image.
void nap() {
  display::sleep(true);
  // Touch: a finger still down would wake the chip at once. Buttons are read on:
  // waiting here would swallow a press made just after the page appeared.
  if (kHasTouch) input::waitAllReleased();
  if (!power::lightSleep(uint32_t(settings.idleMinutes) * 60)) enterSleep(true);
  turnStartMs = millis();
  wokeFromNap = true;
  input::markActivity();
  rtc.wakes++;
  readBattery();
  if (batteryMv > kNoBatteryMv && batteryMv < kBatteryLowMv) {
    flushStats();
    showLowBattery();
    display::sleep(false);
    power::deepSleep(0);
  }
}

// After a page is on the panel: sleep right away, or stay awake (setting).
void pageShown() {
  input::setRepeat(false);
  if (screen != Screen::Reading || !settings.deepSleep) return;
  if (pendingKey != Key::None) return;  // pressed during the refresh: loop() acts on it first
  if (kNapBetweenPages) {
    nap();
  } else {
    enterSleep(false);
  }
}

void resume() {
  showReading(Refresh::Full);  // full: clears the menu's ghosting
  pageShown();
}

void toggleBookmark(uint32_t p) {
  if (!marks.remove(p)) marks.add(p);
  storage::writeMarks(book.stem(), marks);
}

// --- menus -------------------------------------------------------------------

Refresh menuRefresh() {
  return decideRefresh(true, false, display::shownValid(), display::partialsSinceFull(),
                       settings.fullEvery);
}

void mainItem(int i, ui::Item &it, void *) {
  const S id = mainItems()[i];
  strlcpy(it.label, tr(id), sizeof(it.label));
  it.chevron = id != S::Resume && id != S::Sleep && id != S::Lock;
  if (id == S::Resume) snprintf(it.detail, sizeof(it.detail), "%d%%", percentRead());
  if (id == S::Bookmarks && marks.count()) snprintf(it.detail, sizeof(it.detail), "%d", marks.count());
}

void renderMainMenu() {
  String title = book.isOpen() ? String(book.header().title) : prefs::deviceName();
  if (!title.length()) title = tr(S::Menu);
  ui::menu(title.c_str(), list, mainItem, nullptr, listNext(), menuHint(S::Open), batteryPct);
  display::show(menuRefresh());
}

void openMainMenu() {
  input::setRepeat(true);
  list.reset(mainCount(), ui::menuRows(), 0);
  setScreen(Screen::MainMenu);
  renderMainMenu();
}

void chapterItem(int i, ui::Item &it, void *) {
  t5b::TocEntry e;
  strlcpy(it.label, book.tocEntry(i, e) ? e.title : "?", sizeof(it.label));
  snprintf(it.detail, sizeof(it.detail), "%u", unsigned(book.chapterStart(i) + 1));
  it.current = i == book.chapterForPage(page);
}

void renderChapters() {
  ui::menu(tr(S::Chapters), list, chapterItem, nullptr, listNext(), menuHint(S::Go), batteryPct);
  display::show(menuRefresh());
}

void openChapters() {
  const int current = book.chapterForPage(page);
  list.reset(book.tocCount(), ui::menuRows(), current < 0 ? 0 : current);
  setScreen(Screen::Chapters);
  renderChapters();
}

// Bookmarks: row 0 adds or removes the current page, then one row per bookmark
// (its chapter, page number on the right).
void bookmarkItem(int i, ui::Item &it, void *) {
  if (i == 0) {
    strlcpy(it.label, tr(marks.contains(page) ? S::RemoveBookmarkHere : S::BookmarkHere),
            sizeof(it.label));
    return;
  }
  const uint32_t p = marks.at(i - 1);
  t5b::TocEntry e;
  strlcpy(it.label, chapterTitle(p, e), sizeof(it.label));
  snprintf(it.detail, sizeof(it.detail), "%u", unsigned(p + 1));
  it.current = p == page;
}

void renderBookmarks() {
  ui::menu(tr(S::Bookmarks), list, bookmarkItem, nullptr, listNext(), menuHint(S::Go), batteryPct);
  display::show(menuRefresh());
}

void openBookmarks(int selected = 0) {
  input::setRepeat(true);
  list.reset(marks.count() + 1, ui::menuRows(), selected);
  setScreen(Screen::Bookmarks);
  renderBookmarks();
}

uint32_t pctToPage(int pct) { return uint32_t(uint64_t(book.pageCount() - 1) * pct / 100); }

void renderGoTo() {
  char value[8];
  char caption[40];
  snprintf(value, sizeof(value), "%d%%", goToPct);
  snprintf(caption, sizeof(caption), tr(S::PageOf), unsigned(pctToPage(goToPct) + 1),
           unsigned(book.pageCount()));
  ui::valueEditor(tr(S::GoTo), value, caption, goToPct, {Button::Next, "+1", "+10"},
                  menuHint(S::Go), batteryPct);
  display::show(menuRefresh());
}

void openGoTo() {
  goToPct = percentRead();
  setScreen(Screen::GoTo);
  renderGoTo();
}

bool isLandscape(int screen) { return kScreens[screen].w > kScreens[screen].h; }

// A book made for the board's other screen (7.5": portrait vs landscape).
void showOtherScreen(int screen) {
  ui::message(ui::Icon::Book, tr(isLandscape(screen) ? S::MadeForLandscape : S::MadeForPortrait),
              tr(S::OtherScreenLine1), tr(S::OtherScreenLine2), tr(S::BackAnyButton));
  display::show(menuRefresh());
  setScreen(Screen::Message);
  afterMessage = AfterMessage::Library;
}

// The open book lives in this library entry (the book itself or its folder).
bool holdsOpenBook(const storage::BookEntry &e) {
  if (!book.isOpen()) return false;
  if (!e.isFolder) return e.path == book.path();
  const String prefix = e.path + "/";
  return strncmp(book.path(), prefix.c_str(), prefix.length()) == 0;
}

void libraryItem(int i, ui::Item &it, void *) {
  const storage::BookEntry &e = library[i];
  strlcpy(it.label, e.title.c_str(), sizeof(it.label));
  it.current = holdsOpenBook(e);
  if (e.isFolder) {
    it.chevron = true;
    snprintf(it.detail, sizeof(it.detail), tr(S::BooksN), unsigned(e.pages));
    return;
  }
  if (e.screen != display::screenIndex()) {  // made for the other orientation
    strlcpy(it.detail, tr(isLandscape(e.screen) ? S::OrientLandscape : S::OrientPortrait),
            sizeof(it.detail));
    return;
  }
  const uint32_t mark = it.current ? page : prefs::bookmark(e.id);
  if (mark == 0) {
    strlcpy(it.detail, tr(S::New), sizeof(it.detail));
  } else {
    const uint32_t last = e.pages > 1 ? e.pages - 1 : 1;
    snprintf(it.detail, sizeof(it.detail), "%u%%", unsigned(uint64_t(mark) * 100 / last));
  }
}

void renderLibrary() {
  String title = libraryFolder;
  if (!title.length()) title = prefs::deviceName();
  if (!title.length()) title = tr(S::Library);
  // Hold MENU: up a folder, back to the menu, or (no book open) to the short menu.
  const ui::Hint hint = libraryFolder.length() || book.isOpen()
                            ? menuHint(S::Open)
                            : ui::Hint{Button::Menu, tr(S::Open), tr(S::MenuAction)};
  ui::menu(title.c_str(), list, libraryItem, nullptr, listNext(), hint, batteryPct);
  display::show(menuRefresh());
}

// "No books yet": a press starts Wi-Fi transfer, or MENU the book server if one is set.
bool noBooksToServer(Key k) {
  return (k == Key::MenuShort || k == Key::MenuLong) && bookserver::address().length();
}

void showNoBooks() {
  // Any press starts Wi-Fi transfer: the only way forward without a PC (the card
  // is read again when it ends). A "retry" here just restarted to the same screen.
  // Books in the flash: Wi-Fi is the only way in, and the room is small.
  // Text pages compress about 4 to 5 times (deflate, .t5b version 2): count 4.
  char room[48];
  snprintf(room, sizeof(room), tr(S::NoBooksFlash2),
           unsigned(max<int64_t>(storage::freeBytes(), 0) / (kPageBytes / 4)));
  const ui::Hint server = bookserver::address().length()
                              ? ui::Hint{Button::Menu, tr(S::BookServer), nullptr}
                              : kNoHint;
  ui::message(ui::Icon::Book, tr(S::NoBooksYet),
              tr(kBooksInFlash ? S::NoBooksFlash1 : S::NoBooksLine1),
              kBooksInFlash ? room : tr(S::NoBooksLine2),
              {Button::Next, tr(S::WifiTransfer), nullptr}, server);
  display::show(Refresh::Full);
  setScreen(Screen::Message);
  afterMessage = AfterMessage::NoBooks;
}

// One library level; `select`: the title of the row to start on ("" = the open
// book or its folder).
void openLibrary(const String &folder = "", const String &select = "") {
  const storage::Status status = storage::listLibrary(folder.c_str(), library);
  if (status != storage::Status::Ok) {
    if (folder.length()) {
      openLibrary();  // the folder is gone or empty
      return;
    }
    showNoBooks();
    return;
  }
  libraryFolder = folder;
  int selected = 0;
  for (size_t i = 0; i < library.size(); i++) {
    if (select.length() ? library[i].title == select : holdsOpenBook(library[i])) selected = int(i);
  }
  input::setRepeat(true);
  list.reset(int(library.size()), ui::menuRows(), selected);
  setScreen(Screen::Library);
  renderLibrary();
}

void openStatistics() {
  const uint32_t now = clockNow();
  const int32_t tz = prefs::timeZoneOffset();
  const ReadingStats &st = rtc.stats;
  char v[4][72];
  if (now >= kClockValid) {
    snprintf(v[0], sizeof(v[0]), tr(S::PagesMinutes), unsigned(todayPages(st, now, tz)),
             unsigned(todaySeconds(st, now, tz) / 60));
  } else {
    strlcpy(v[0], tr(S::ClockNotSet), sizeof(v[0]));
  }
  snprintf(v[1], sizeof(v[1]), tr(S::StreakValue), unsigned(currentStreak(st, now, tz)),
           unsigned(st.bestStreak));
  snprintf(v[2], sizeof(v[2]), "%u", unsigned(st.daysRead));
  snprintf(v[3], sizeof(v[3]), tr(S::PagesHours), unsigned(st.pages), unsigned(st.seconds / 3600),
           unsigned(st.seconds / 60 % 60));
  const char *labels[] = {tr(S::Today), tr(S::Streak), tr(S::DaysRead), tr(S::Total)};
  const char *values[] = {v[0], v[1], v[2], v[3]};
  ui::details(tr(S::Statistics), labels, values, 4, kNoHint, backHint(), batteryPct);
  display::show(menuRefresh());
  setScreen(Screen::Statistics);
}

void settingItem(int i, ui::Item &it, void *) {
  strlcpy(it.label, settingLabel(SettingId(i), ui::holdSettingLabel()), sizeof(it.label));
  settingValue(settings, SettingId(i), it.detail, sizeof(it.detail));
}

void renderSettings() {
  ui::menu(tr(S::Settings), list, settingItem, nullptr, listNext(), menuHint(S::Edit), batteryPct);
  display::show(menuRefresh());
}

void openSettings() {
  list.reset(kSettingCount, ui::menuRows(), 0);
  setScreen(Screen::SettingsMenu);
  renderSettings();
}

void openAbout() {
  char v[6][72];
  snprintf(v[0], sizeof(v[0]), "%s  \xC2\xB7  %s", FW_VERSION, BOARD_NAME);
  if (batteryPct >= 0) {
    snprintf(v[1], sizeof(v[1]), "%u.%02u V  \xC2\xB7  %d%%", unsigned(batteryMv / 1000),
             unsigned(batteryMv % 1000 / 10), batteryPct);
  } else {
    snprintf(v[1], sizeof(v[1]), "%s", tr(S::UsbPower));
  }
  if (book.isOpen()) {
    snprintf(v[2], sizeof(v[2]), "%s", book.header().title);
    snprintf(v[3], sizeof(v[3]), tr(S::PositionValue), unsigned(page + 1),
             unsigned(book.pageCount()), book.chapterForPage(page) + 1, book.tocCount());
  } else {
    strlcpy(v[2], "-", sizeof(v[2]));
    strlcpy(v[3], "-", sizeof(v[3]));
  }
  char card[16] = "-";
  const int64_t room = storage::freeBytes();
  if (room >= 0) formatBytes(uint64_t(room), card, sizeof(card));
  snprintf(v[4], sizeof(v[4]), tr(kBooksInFlash ? S::MemoryFlashValue : S::MemoryValue), card,
           unsigned(ESP.getFreeHeap() / 1024));
  snprintf(v[5], sizeof(v[5]), tr(S::WakeValue), unsigned(rtc.imageMs), unsigned(rtc.wakes));
  const char *labels[] = {tr(S::Firmware), tr(S::Battery), tr(S::Book),
                          tr(S::Position), tr(S::Memory),  tr(S::WakeToPage)};
  const char *values[] = {v[0], v[1], v[2], v[3], v[4], v[5]};
  ui::details(tr(S::About), labels, values, 6, kNoHint, backHint(), batteryPct);
  display::show(menuRefresh());
  setScreen(Screen::About);
}

// --- Wi-Fi transfer ---------------------------------------------------------------

bool restoreBook();

int transferStep() {
  const transfer::Info &t = transfer::info();
  if (!t.ownNetwork) return 0;  // home network: only the page QR
  if (wifiStep) return wifiStep;
  return t.clients ? 2 : 1;
}

void renderTransfer() {
  const transfer::Info &t = transfer::info();
  const int step = transferStep();
  char qr[128];
  ui::WifiView v;
  if (step == 1) {
    wifiQrPayload(t.ssid, t.password, qr, sizeof(qr));
    v = {qr, 1, tr(S::JoinWifi), tr(S::Network), t.ssid, tr(S::Password), t.password, t.event};
  } else {
    strlcpy(qr, t.url, sizeof(qr));
    v = {qr, step, tr(S::OpenThePage), tr(S::Address), t.address, tr(S::Code), t.code, t.event};
  }
  const ui::Hint other = {Button::Next, tr(step == 1 ? S::ShowPage : S::ShowWifi), nullptr};
  ui::wifiScreen(v, step ? other : kNoHint, {Button::Menu, tr(S::Done), nullptr}, batteryPct);
  display::show(menuRefresh());
}

void startTransfer() {
  flushStats();
  book.close();  // uploads may replace or delete it; reopened when done
  input::setRepeat(false);
  wifiStep = 0;
  transfer::start(batteryPct);
  transfer::takeChanged();
  setScreen(Screen::WifiTransfer);
  renderTransfer();
}

// After Wi-Fi: the book again, from NVS, not the RTC page: it may have been
// restyled to a new page count, moved to a folder, or given another position.
void reopenBook() {
  const String last = prefs::lastBook();
  if (!(last.length() && openBook(last.c_str(), true)) &&
      !(rtc.bookPath[0] && openBook(rtc.bookPath, true))) {
    restoreBook();
  }
}

void stopTransfer() {
  transfer::stop();
  // The page may have changed settings, the lock, the statistics or the name.
  settings = prefs::loadSettings(boardDefaults());
  applySettings();
  locked = prefs::locked();
  rtc.stats = prefs::loadStats();
  rtc.statsUnsaved = 0;
  reopenBook();
  openLibrary(libraryFolder);  // shows what is on the card now
}

void transferKey(Key k) {
  switch (k) {
    case Key::NextShort:
      if (transferStep()) {
        wifiStep = transferStep() == 1 ? 2 : 1;
        renderTransfer();
      }
      break;
    case Key::MenuShort:
    case Key::MenuLong: stopTransfer(); break;
    default: break;
  }
}


// --- book server ------------------------------------------------------------------
// Menu -> Book server: join the home Wi-Fi, pick a profile (if the server has more
// than one for this screen), then Newest / Authors / Titles, a book; the server
// typesets it (progress shown), the reader downloads it, in parts if it is too big.

enum class ServerView : uint8_t { Profiles, Top, Newest, Titles, Authors, AuthorBooks, Parts };
const S kServerTop[] = {S::Newest, S::Authors, S::Titles};
const ServerView kServerTopViews[] = {ServerView::Newest, ServerView::Authors, ServerView::Titles};
constexpr uint32_t kServerWindow = 40;          // list rows kept (fetched as you scroll)
constexpr uint32_t kServerPollMs = 2000;       // asking how far the typesetting is
constexpr uint32_t kServerGiveUpMs = 20 * 60000;  // a conversion that never ends

struct ServerState {
  std::vector<bookserver::Row> profiles;
  int profile = 0;
  ServerView view = ServerView::Top;
  // the list on screen (Newest, Titles, Authors, AuthorBooks): a window of it
  bool fetched = false;  // total and win are the server's
  uint32_t total = 0;
  uint32_t winStart = 0;
  std::vector<bookserver::Row> win;
  bookserver::Row author;  // AuthorBooks: whose
  int topSelected = 0, authorSelected = 0, listSelected = 0;  // to come back to
  ServerView bookFrom = ServerView::Newest;
  bookserver::Row book;  // being typeset / downloaded
  bookserver::Prepared prep;
  String dir;         // where it goes: where a book of that name is, else /books
  uint32_t room = 0;  // bytes it may take (0: unknown)
  int havePart = -1;  // Parts: the one already on the reader
  uint32_t pollMs = 0, busySinceMs = 0, drawnMs = 0;
  int shownPercent = -1;
  String saved;                 // the book just downloaded
  std::vector<String> onReader;  // file names in the library (marked in lists)
};
ServerState srv;

const bookserver::Row &serverProfile() { return srv.profiles[srv.profile]; }

bool windowed(ServerView v) {
  return v == ServerView::Newest || v == ServerView::Titles || v == ServerView::Authors ||
         v == ServerView::AuthorBooks;
}

bool onReader(const char *file) {
  for (const String &f : srv.onReader) {
    if (f == file) return true;
  }
  return false;
}

void serverMessage(ui::Icon icon, const char *headline, const char *line1, const char *line2,
                   AfterMessage after) {
  ui::message(icon, headline, line1, line2, tr(S::BackAnyButton));
  display::show(menuRefresh());
  setScreen(Screen::Message);
  afterMessage = after;
  input::setRepeat(false);
}

// The server stopped answering: say so, and any key leaves the book server.
void serverLost() {
  serverMessage(ui::Icon::Warning, tr(S::ServerUnreachable), bookserver::host(),
                bookserver::error(), AfterMessage::LeaveServer);
}

void leaveServer(String open = "") {  // a copy: it may be srv.saved, cleared below
  bookserver::disconnect();
  srv = ServerState();  // frees the lists
  if (open.length() && openBook(open.c_str())) {
    resume();
    return;
  }
  reopenBook();
  openLibrary(libraryFolder);  // with what was downloaded
}

// Rows `first` .. first+count-1 of the list on screen (as far as it goes), fetched if
// not there yet. False: the server didn't answer.
bool serverRows(uint32_t first, uint32_t count) {
  if (srv.fetched) {
    const uint32_t end = min(first + count, srv.total);
    if (first >= end || (first >= srv.winStart && end <= srv.winStart + srv.win.size())) {
      return true;
    }
  }
  const uint32_t start = first > kServerWindow / 4 ? first - kServerWindow / 4 : 0;
  char query[48];
  const char *what = srv.view == ServerView::Authors ? "authors" : "books";
  switch (srv.view) {
    case ServerView::Titles: strlcpy(query, "sort=title", sizeof(query)); break;
    case ServerView::AuthorBooks: snprintf(query, sizeof(query), "author=%s", srv.author.id); break;
    default: strlcpy(query, "sort=new", sizeof(query)); break;
  }
  uint32_t total = 0;
  std::vector<bookserver::Row> rows;
  if (!bookserver::list(what, query, start, max(kServerWindow, count), total, rows)) return false;
  srv.fetched = true;
  srv.total = total;
  srv.winStart = start;
  srv.win.swap(rows);
  return true;
}

bool haveRow(int i) {
  return uint32_t(i) >= srv.winStart && uint32_t(i) < srv.winStart + srv.win.size();
}

int serverCount() {
  switch (srv.view) {
    case ServerView::Profiles: return int(srv.profiles.size());
    case ServerView::Top: return 3;
    case ServerView::Parts: return int(srv.prep.parts.size());
    default: return int(srv.total);
  }
}

void serverItem(int i, ui::Item &it, void *) {
  switch (srv.view) {
    case ServerView::Profiles:
      strlcpy(it.label, srv.profiles[i].title, sizeof(it.label));
      it.current = i == srv.profile;
      it.chevron = true;
      return;
    case ServerView::Top:
      strlcpy(it.label, tr(kServerTop[i]), sizeof(it.label));
      it.chevron = true;
      return;
    case ServerView::Parts: {
      const bookserver::Part &part = srv.prep.parts[i];
      snprintf(it.label, sizeof(it.label), tr(S::PagesRange), unsigned(part.first),
               unsigned(part.last));
      formatBytes(part.bytes, it.detail, sizeof(it.detail));
      it.current = i == srv.havePart;
      return;
    }
    default: break;
  }
  if (!haveRow(i)) return;
  const bookserver::Row &r = srv.win[i - srv.winStart];
  strlcpy(it.label, r.title, sizeof(it.label));
  if (srv.view == ServerView::Authors) {
    strlcpy(it.detail, r.detail, sizeof(it.detail));  // number of books
    it.chevron = true;
    return;
  }
  it.current = onReader(r.file);  // the dot: already on the reader
  // The author beside the title where there is room (the 7.5" in landscape): on
  // narrower screens it would cut the titles short.
  if (srv.view != ServerView::AuthorBooks && display::screen().w >= 700 && r.detail[0]) {
    constexpr size_t kAuthorBytes = 24;
    if (strlen(r.detail) <= kAuthorBytes) {
      strlcpy(it.detail, r.detail, sizeof(it.detail));
    } else {
      size_t n = kAuthorBytes - 3;  // and an ellipsis (3 bytes)
      while (n && (uint8_t(r.detail[n]) & 0xC0) == 0x80) n--;  // whole characters
      memcpy(it.detail, r.detail, n);
      strlcpy(it.detail + n, "\xE2\x80\xA6", sizeof(it.detail) - n);
    }
  }
}

const char *serverTitle() {
  switch (srv.view) {
    case ServerView::Profiles: return tr(S::ChooseProfile);
    case ServerView::Top:
      return srv.profiles.size() > 1 ? serverProfile().title : tr(S::BookServer);
    case ServerView::Newest: return tr(S::Newest);
    case ServerView::Titles: return tr(S::Titles);
    case ServerView::Authors: return tr(S::Authors);
    case ServerView::AuthorBooks: return srv.author.title;
    case ServerView::Parts: return tr(S::ChooseAPart);
  }
  return "";
}

void renderServerList() {
  if (windowed(srv.view) && list.count() &&
      !serverRows(uint32_t(list.first()), uint32_t(list.visibleRows()))) {
    serverLost();
    return;
  }
  ui::menu(serverTitle(), list, serverItem, nullptr, listNext(), menuHint(S::Open), batteryPct);
  display::show(menuRefresh());
}

void openServerView(ServerView view, int selected = 0) {
  srv.view = view;
  srv.fetched = false;
  srv.win.clear();
  srv.winStart = 0;
  srv.total = 0;
  if (windowed(view) && !serverRows(uint32_t(max(selected, 0)), 1)) {
    serverLost();
    return;
  }
  input::setRepeat(true);
  list.reset(serverCount(), ui::menuRows(), selected < serverCount() ? selected : 0);
  setScreen(Screen::ServerList);
  renderServerList();
}

void renderTypesetting() {
  char value[8], caption[72];
  snprintf(value, sizeof(value), "%d%%", srv.prep.percent);
  if (srv.prep.ahead > 0) {
    snprintf(caption, sizeof(caption), tr(S::AheadInLine), unsigned(srv.prep.ahead));
  } else {
    strlcpy(caption, srv.book.title, sizeof(caption));
  }
  ui::valueEditor(tr(S::Typesetting), value, caption, srv.prep.percent, kNoHint, backHint(),
                  batteryPct);
  display::show(menuRefresh());
  srv.shownPercent = srv.prep.percent;
}

void renderDownloading(uint32_t done, uint32_t total) {
  const int pct = total ? int(uint64_t(done) * 100 / total) : 0;
  char value[8], a[16], b[16], caption[48];
  snprintf(value, sizeof(value), "%d%%", pct);
  formatBytes(done, a, sizeof(a));
  formatBytes(total, b, sizeof(b));
  snprintf(caption, sizeof(caption), tr(S::BytesOf), a, b);
  ui::valueEditor(tr(S::Downloading), value, caption, pct, kNoHint, kNoHint, batteryPct);
  display::show(menuRefresh());
  srv.shownPercent = pct;
  srv.drawnMs = millis();
}

// Download progress: redrawn every fifth of the way at most (a refresh takes a second).
void downloadProgress(uint32_t done, uint32_t total, void *) {
  const int pct = total ? int(uint64_t(done) * 100 / total) : 0;
  if (pct / 20 > srv.shownPercent / 20 && pct < 100 && millis() - srv.drawnMs > 1500) {
    renderDownloading(done, total);
  }
  input::markActivity();
}

// Downloads the book (or pages first..last of it) and stores it where it goes.
void downloadBook(uint32_t first, uint32_t last, uint32_t bytes) {
  const String path = srv.dir + "/" + srv.book.file;
  const String tmp = path + ".part";
  // The chosen book needs the room of the one it replaces (the next part of a book
  // on a reader without a card): that one goes first.
  const int64_t room = storage::freeBytes();
  if (room >= 0 && int64_t(bytes) + int64_t(catalog::kRoomMargin) > room &&
      storage::files().exists(path)) {
    Serial.printf("book server: removing %s to make room\n", path.c_str());
    storage::files().remove(path);
  }
  storage::files().mkdir(storage::kBooksDir);
  srv.shownPercent = -1;
  renderDownloading(0, bytes);
  if (!bookserver::download(serverProfile().id, srv.book.id, first, last, tmp, downloadProgress,
                            nullptr)) {
    serverMessage(ui::Icon::Warning, tr(S::DownloadFailed), srv.book.title, bookserver::error(),
                  AfterMessage::ServerList);
    return;
  }
  t5b::Header h;
  const char *why = "";
  if (transfer::installBook(tmp, path, transfer::KeepPosition::SameTitle, h, why) !=
      transfer::Install::Ok) {
    serverMessage(ui::Icon::Warning, tr(S::DownloadFailed), srv.book.title, why,
                  AfterMessage::ServerList);
    return;
  }
  Serial.printf("book server: saved %s (%u pages)\n", path.c_str(), unsigned(h.pageCount));
  srv.saved = path;
  if (!onReader(srv.book.file)) srv.onReader.push_back(srv.book.file);
  ui::message(ui::Icon::Book, tr(S::Downloaded), h.title, h.author,
              {Button::Next, tr(S::ReadAction), nullptr}, {Button::Menu, tr(S::MoreAction), nullptr});
  display::show(menuRefresh());
  setScreen(Screen::ServerDone);
  input::setRepeat(false);
}

// The parts that fit, starting on the one after the part already on the reader.
void openParts() {
  srv.havePart = -1;
  fs::File f = storage::files().open(srv.dir + "/" + srv.book.file, FILE_READ);
  uint8_t head[t5b::kHeaderSize];
  t5b::Header h;
  if (f && f.read(head, sizeof(head)) == sizeof(head) &&
      t5b::parseHeader(head, sizeof(head), h) == t5b::Error::None) {
    for (size_t i = 0; i < srv.prep.parts.size(); i++) {
      if (catalog::isPartTitle(h.title, srv.prep.parts[i].first, srv.prep.parts[i].last)) {
        srv.havePart = int(i);
      }
    }
  }
  if (f) f.close();
  const int next = srv.havePart + 1 < int(srv.prep.parts.size()) ? srv.havePart + 1 : 0;
  openServerView(ServerView::Parts, next);
}

// Asks the server how far the book is; acts when it is ready or failed.
void pollTypesetting() {
  srv.pollMs = millis();
  input::markActivity();  // waiting for the server is not idle
  if (!bookserver::prepare(serverProfile().id, srv.book.id, srv.room, srv.prep)) {
    if (srv.prep.state == bookserver::Prepared::State::Failed && bookserver::error()[0] &&
        strcmp(bookserver::error(), tr(S::NoAnswer)) != 0) {
      serverMessage(ui::Icon::Warning, tr(S::CannotTypeset), srv.book.title, bookserver::error(),
                    AfterMessage::ServerList);
    } else {
      serverLost();
    }
    return;
  }
  switch (srv.prep.state) {
    case bookserver::Prepared::State::Busy:
      if (millis() - srv.busySinceMs > kServerGiveUpMs) {
        serverMessage(ui::Icon::Warning, tr(S::CannotTypeset), srv.book.title, tr(S::TookTooLong),
                      AfterMessage::ServerList);
      } else if (srv.prep.percent != srv.shownPercent) {
        renderTypesetting();
      }
      break;
    case bookserver::Prepared::State::Failed:
      serverMessage(ui::Icon::Warning, tr(S::CannotTypeset), srv.book.title, bookserver::error(),
                    AfterMessage::ServerList);
      break;
    case bookserver::Prepared::State::Ready:
      if (!srv.room || srv.prep.bytes <= srv.room) {
        downloadBook(0, 0, srv.prep.bytes);
      } else if (srv.prep.parts.empty()) {
        char free[16], line[48];
        formatBytes(srv.room, free, sizeof(free));
        snprintf(line, sizeof(line), tr(S::RoomFree), free);
        serverMessage(ui::Icon::Warning, tr(S::NotEnoughRoom), srv.book.title, line,
                      AfterMessage::ServerList);
      } else {
        openParts();
      }
      break;
  }
}

void startTypesetting(const bookserver::Row &row) {
  srv.book = row;
  srv.dir = storage::bookDirFor(row.file);
  uint32_t replaced = 0;
  fs::File old = storage::files().open(srv.dir + "/" + row.file, FILE_READ);
  if (old) {
    replaced = uint32_t(old.size());
    old.close();
  }
  srv.room = catalog::downloadRoom(storage::freeBytes(), replaced);
  srv.busySinceMs = millis();
  srv.shownPercent = -1;
  srv.prep = bookserver::Prepared();
  input::setRepeat(false);
  setScreen(Screen::ServerBusy);
  pollTypesetting();
}

void startServer() {
  flushStats();
  book.close();  // a download may replace it; reopened when done
  input::setRepeat(false);
  srv = ServerState();
  const String address = bookserver::address();
  ui::message(ui::Icon::Book, tr(S::BookServer), tr(S::Connecting), address.c_str(), nullptr);
  display::show(menuRefresh());
  setScreen(Screen::ServerBusy);  // no keys until the next screen
  char ssid[33];
  switch (bookserver::connect(ssid, sizeof(ssid))) {
    case bookserver::Join::Ok: break;
    case bookserver::Join::NoServerSet:
      return serverMessage(ui::Icon::Warning, tr(S::NoServerSet), tr(S::SetOnPage1),
                           tr(S::SetOnPage2), AfterMessage::LeaveServer);
    case bookserver::Join::BadAddress:
      return serverMessage(ui::Icon::Warning, tr(S::BadAddress), address.c_str(),
                           tr(S::SetOnPage2), AfterMessage::LeaveServer);
    case bookserver::Join::NoWifiSet:
      return serverMessage(ui::Icon::Warning, tr(S::NoHomeWifi), tr(S::SetOnPage1),
                           tr(S::SetOnPage2), AfterMessage::LeaveServer);
    case bookserver::Join::WifiFailed:
      return serverMessage(ui::Icon::Warning, tr(S::WifiNotJoined), ssid, tr(S::OutOfReach),
                           AfterMessage::LeaveServer);
  }
  std::vector<storage::BookEntry> books;
  storage::listAllBooks(books);
  for (const storage::BookEntry &b : books) {
    srv.onReader.push_back(b.path.substring(b.path.lastIndexOf('/') + 1));
  }
  if (!bookserver::profiles(display::screen().id, srv.profiles)) return serverLost();
  if (srv.profiles.empty()) {
    return serverMessage(ui::Icon::Warning, tr(S::BookServer), display::screen().id,
                         tr(S::NothingHere), AfterMessage::LeaveServer);
  }
  if (srv.profiles.size() > 1) {
    openServerView(ServerView::Profiles);
  } else {
    openServerView(ServerView::Top);
  }
}

// Back from a server list: up a level, out of the server at the top.
void serverBack() {
  switch (srv.view) {
    case ServerView::Profiles: leaveServer(); break;
    case ServerView::Top:
      if (srv.profiles.size() > 1) {
        openServerView(ServerView::Profiles, srv.profile);
      } else {
        leaveServer();
      }
      break;
    case ServerView::AuthorBooks: openServerView(ServerView::Authors, srv.authorSelected); break;
    case ServerView::Parts: openServerView(srv.bookFrom, srv.listSelected); break;
    default: openServerView(ServerView::Top, srv.topSelected); break;
  }
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
    case Key::None:
    case Key::Touch: break;  // mapped to keys by touchKey()
  }
  return ListAction::None;
}

void serverListKey(Key k) {
  switch (listKey(k)) {
    case ListAction::Moved: renderServerList(); break;
    case ListAction::Back: serverBack(); break;
    case ListAction::Select: {
      const int i = list.selected();
      if (!list.count()) break;
      switch (srv.view) {
        case ServerView::Profiles:
          srv.profile = i;
          openServerView(ServerView::Top, srv.topSelected);
          break;
        case ServerView::Top:
          srv.topSelected = i;
          openServerView(kServerTopViews[i]);
          break;
        case ServerView::Parts: {
          const bookserver::Part part = srv.prep.parts[i];
          downloadBook(part.first, part.last, part.bytes);
          break;
        }
        default: {
          if (!serverRows(uint32_t(i), 1)) return serverLost();
          if (!haveRow(i)) break;
          const bookserver::Row row = srv.win[i - srv.winStart];
          if (srv.view == ServerView::Authors) {
            srv.author = row;
            srv.authorSelected = i;
            openServerView(ServerView::AuthorBooks);
          } else {
            srv.listSelected = i;
            srv.bookFrom = srv.view;
            startTypesetting(row);
          }
          break;
        }
      }
      break;
    }
    case ListAction::None: break;
  }
}

void serverBusyKey(Key k) {
  if (k == Key::MenuShort || k == Key::MenuLong) {  // stop waiting (the server goes on)
    openServerView(srv.bookFrom, srv.listSelected);
  }
}

void serverDoneKey(Key k) {
  switch (k) {
    case Key::NextShort:
    case Key::NextLong: leaveServer(srv.saved); break;  // read it
    case Key::MenuShort:
    case Key::MenuLong: openServerView(srv.bookFrom, srv.listSelected); break;  // more
    default: break;
  }
}

// The hold gesture while reading (Settings: Hold MENU / Long hold / Long press).
void holdAction() {
  switch (settings.holdAction) {
    case HoldAction::Sleep: enterSleep(true);
    case HoldAction::Bookmark:
      toggleBookmark(page);
      showReading(Refresh::Partial);
      pageShown();
      break;
    case HoldAction::Lock:
      setLocked(true);
      showReading(Refresh::Partial);
      pageShown();
      break;
    case HoldAction::Menu: openMainMenu(); break;
  }
}

void readingKey(Key k) {
  if (locked) {  // only the unlock gesture does something
    if (k == Key::MenuLong) {
      setLocked(false);
      showReading(Refresh::Partial);
    }
    pageShown();
    return;
  }
  switch (k) {
    case Key::NextShort:
      if (page + 1 < book.pageCount()) {
        page++;
        recordPage(rtc.stats, clockNow(), prefs::timeZoneOffset());
        if (++rtc.statsUnsaved >= kStatsFlushPages) flushStats();
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
    case Key::MenuLong: holdAction(); break;
    default: break;
  }
}

// Out of the menu: the open book, or the library when there is none.
void backToBook() {
  if (book.isOpen()) {
    resume();
  } else {
    openLibrary(libraryFolder);
  }
}

void mainMenuKey(Key k) {
  switch (listKey(k)) {
    case ListAction::Moved: renderMainMenu(); break;
    case ListAction::Back: backToBook(); break;
    case ListAction::Select:
      switch (mainItems()[list.selected()]) {
        case S::Chapters: openChapters(); break;
        case S::Bookmarks: openBookmarks(); break;
        case S::GoTo: openGoTo(); break;
        case S::Library: openLibrary(); break;
        case S::Statistics: openStatistics(); break;
        case S::WifiTransfer: startTransfer(); break;
        case S::BookServer: startServer(); break;
        case S::Settings: openSettings(); break;
        case S::About: openAbout(); break;
        case S::Lock:
          setLocked(true);
          resume();
          break;
        case S::Sleep: enterSleep(true);
        default: backToBook(); break;
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

void bookmarksKey(Key k) {
  switch (listKey(k)) {
    case ListAction::Moved: renderBookmarks(); break;
    case ListAction::Back: openMainMenu(); break;
    case ListAction::Select:
      if (list.selected() == 0) {
        toggleBookmark(page);
        openBookmarks(0);
      } else {
        page = marks.at(list.selected() - 1);
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
    case Key::None:
    case Key::Touch: break;
  }
}

void libraryKey(Key k) {
  switch (listKey(k)) {
    case ListAction::Moved: renderLibrary(); break;
    case ListAction::Back:
      if (libraryFolder.length()) {
        openLibrary("", libraryFolder);  // up to /books, on this folder
      } else {
        openMainMenu();  // without a book: the short menu (Wi-Fi transfer, Settings, ...)
      }
      break;
    case ListAction::Select: {
      const storage::BookEntry e = library[list.selected()];
      if (e.isFolder) {
        openLibrary(e.title);
      } else if (e.screen != display::screenIndex()) {
        showOtherScreen(e.screen);
      } else if (book.isOpen() && e.path == book.path()) {
        resume();
      } else if (openBook(e.path.c_str())) {
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
      applySettings();
      if (book.isOpen() && book.screen() != display::screenIndex()) {
        // Turned to the other screen: the open book was made for this one.
        book.close();
        rtc.bookPath[0] = '\0';
        openLibrary(libraryFolder);
        break;
      }
      renderSettings();
      break;
    case ListAction::None: break;
  }
}

bool isList(Screen s) {
  return s == Screen::MainMenu || s == Screen::Chapters || s == Screen::Bookmarks ||
         s == Screen::Library || s == Screen::SettingsMenu || s == Screen::ServerList;
}

void renderList() {
  switch (screen) {
    case Screen::MainMenu: renderMainMenu(); break;
    case Screen::Chapters: renderChapters(); break;
    case Screen::Bookmarks: renderBookmarks(); break;
    case Screen::Library: renderLibrary(); break;
    case Screen::SettingsMenu: renderSettings(); break;
    case Screen::ServerList: renderServerList(); break;
    default: break;
  }
}

// Touch boards: what a gesture means on the current screen. Reading: tap zones
// (left third back, top strip menu, rest next), swipes, long press = the hold
// action (menu by default). Elsewhere: rows and footer buttons as drawn
// (ui::hit), swipe up/down pages through a list, long press = back, a tap
// anywhere on a message = any key.
Key touchKey(const TouchEvent &t) {
  if (screen == Screen::Reading) {
    switch (t.kind) {
      case TouchKind::SwipeLeft: return Key::NextShort;
      case TouchKind::SwipeRight: return Key::NextLong;
      case TouchKind::LongPress: return Key::MenuLong;
      case TouchKind::Tap:
        switch (readingTap(t.x, t.y, display::screen().w, display::screen().h)) {
          case ReadingTap::Next: return Key::NextShort;
          case ReadingTap::Prev: return Key::NextLong;
          case ReadingTap::Menu: return Key::MenuShort;
        }
        break;
      default: break;
    }
    return Key::None;
  }
  if (t.kind == TouchKind::LongPress) return Key::MenuLong;
  if (isList(screen) && (t.kind == TouchKind::SwipeUp || t.kind == TouchKind::SwipeDown)) {
    int i = list.selected() + (t.kind == TouchKind::SwipeUp ? 1 : -1) * list.visibleRows();
    if (i >= list.count()) i = list.count() - 1;
    if (i < 0) i = 0;
    list.select(i);
    renderList();
    return Key::None;
  }
  if (t.kind != TouchKind::Tap) return Key::None;
  const ui::Hit h = ui::hit(t.x, t.y);
  switch (h.kind) {
    case ui::Hit::Chip:
      if (h.button == Button::Next) return h.second ? Key::NextLong : Key::NextShort;
      return h.second ? Key::MenuLong : Key::MenuShort;
    case ui::Hit::Row:
      list.select(h.row);
      return Key::MenuShort;  // open / select / change that row
    case ui::Hit::Bar:
      if (screen == Screen::GoTo) {
        goToPct = h.percent;
        renderGoTo();
      }
      return Key::None;
    case ui::Hit::None: break;
  }
  const bool anyKey = screen == Screen::Message || screen == Screen::About ||
                      screen == Screen::Statistics || screen == Screen::Sleeping ||
                      screen == Screen::LowBattery || screen == Screen::None;
  return anyKey ? Key::NextShort : Key::None;
}

void handle(Key k) {
  if (k == Key::Touch) k = touchKey(input::lastTouch());
  if (k == Key::None) return;
  if (!wokeFromNap) turnStartMs = millis();  // after a nap: counted from the wake
  wokeFromNap = false;
  Serial.printf("key %d on screen %d\n", int(k), int(screen));
  switch (screen) {
    case Screen::Reading: readingKey(k); break;
    case Screen::MainMenu: mainMenuKey(k); break;
    case Screen::Chapters: chaptersKey(k); break;
    case Screen::Bookmarks: bookmarksKey(k); break;
    case Screen::GoTo: goToKey(k); break;
    case Screen::Library: libraryKey(k); break;
    case Screen::SettingsMenu: settingsKey(k); break;
    case Screen::About:
    case Screen::Statistics: openMainMenu(); break;
    case Screen::WifiTransfer: transferKey(k); break;
    case Screen::ServerList: serverListKey(k); break;
    case Screen::ServerBusy: serverBusyKey(k); break;
    case Screen::ServerDone: serverDoneKey(k); break;
    case Screen::Message:
      switch (afterMessage) {
        case AfterMessage::NoBooks:
          if (noBooksToServer(k)) {
            startServer();
          } else {
            startTransfer();
          }
          break;
        case AfterMessage::Library: openLibrary(libraryFolder); break;
        case AfterMessage::ServerList: openServerView(srv.bookFrom, srv.listSelected); break;
        case AfterMessage::LeaveServer: leaveServer(); break;
        case AfterMessage::Restart: ESP.restart();  // retry from the start (card inserted, ...)
      }
      break;
    case Screen::Sleeping:
    case Screen::LowBattery:
    case Screen::None:
      if (locked && k != Key::MenuLong) break;  // locked: only the unlock gesture
      if (locked) setLocked(false);
      backToBook();
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
  power::begin();  // M5Paper: hold the power on before anything else
  Serial.begin(115200);
  const power::Wake wake = power::wakeReason();
  prefs::begin();
  setOrientationCount(kScreenCount > 1 ? 4 : 2);  // before loading: the 7.5" turns
  settings = prefs::loadSettings(boardDefaults());
  setLanguage(settings.language);
  locked = prefs::locked();
  input::begin(settings.longPressMs);
  input::setFlipped(settings.orientation & 1);  // which button the wake press was
  display::setIdleHook(pollWhileRefreshing);
  rtcclock::restore();  // M5 boards: the clock after switching off
  Key key = Key::None;
  if (wake == power::Wake::NextButton) key = input::wakePress(kBtnNext);
  if (wake == power::Wake::MenuButton) key = input::wakePress(kBtnMenu);
  if (wake == power::Wake::PrevButton) key = input::wakePress(kBtnPrev);

  if (rtc.magic != kRtcMagic) {
    memset(&rtc, 0, sizeof(rtc));
    rtc.magic = kRtcMagic;
    rtc.chapter = -1;
    rtc.stats = prefs::loadStats();
  }
  rtc.wakes++;
  const Screen shown = rtc.screen;
  readBattery();
  Serial.printf("\nT5 reader %s (%s): wake by %s, key %d, battery %u mV, shown screen %d%s\n",
                FW_VERSION, BOARD_NAME, power::wakeName(wake), int(key), unsigned(batteryMv),
                int(shown), locked ? ", locked" : "");

  display::begin();
  applySettings();  // after display::begin(): the screen orientation
  // Locked and woken by a press that doesn't unlock: back to sleep, screen untouched.
  const bool buttonWake = wake == power::Wake::NextButton || wake == power::Wake::MenuButton ||
                          wake == power::Wake::PrevButton;
  if (locked && buttonWake && key != Key::MenuLong &&
      (shown == Screen::Reading || shown == Screen::Sleeping)) {
    sleepAgain();
  }
  if (batteryMv > kNoBatteryMv && batteryMv < kBatteryLowMv) {
    flushStats();
    if (shown != Screen::LowBattery) showLowBattery();
    display::sleep(false);
    power::deepSleep(0);
  }
  if (!storage::mount()) {
    if (kBooksInFlash) {  // not this env's partition table (flashed the app alone?)
      showMessage(ui::Icon::Card, tr(S::NoFlash), tr(S::NoFlashLine1), tr(S::NoFlashLine2));
    } else {
      showMessage(ui::Icon::Card, tr(S::NoCard), tr(S::NoCardLine1), tr(S::NoCardLine2));
    }
    return;
  }
  if (!restoreBook()) {
    // first start, or the last book is gone. A press that woke "No books yet"
    // asks for Wi-Fi transfer (it's the only thing that screen offers).
    if (key != Key::None && shown == Screen::Message) {
      std::vector<storage::BookEntry> any;
      const storage::Status status = storage::listAllBooks(any, 1);
      if (status == storage::Status::NoBooks || status == storage::Status::NoBooksDir) {
        if (noBooksToServer(key)) {
          startServer();
        } else {
          startTransfer();
        }
        return;
      }
    }
    openLibrary();
    return;
  }

  if (wake == power::Wake::Timer) enterSleep(true);  // idle while a page was shown
  if (locked && key == Key::MenuLong) {  // the unlock gesture woke us
    setLocked(false);
    resume();
    return;
  }
  if (shown != Screen::Reading || key == Key::None) {
    // power-on, or waking from the sleep/low-battery/message screen: no page turn
    resume();
    return;
  }
  readingKey(key);
}

void loop() {
  const bool wifi = screen == Screen::WifiTransfer;
  if (wifi) {
    transfer::loop();
    if (transfer::takeChanged()) renderTransfer();
  }
  if (screen == Screen::ServerBusy && srv.book.id[0] && millis() - srv.pollMs > kServerPollMs) {
    pollTypesetting();
  }
  const Key waiting = pendingKey;
  pendingKey = Key::None;
  handle(waiting != Key::None ? waiting : input::poll());
  uint32_t last = input::lastActivityMs();
  if (wifi && int32_t(transfer::info().lastActivityMs - last) > 0) last = transfer::info().lastActivityMs;
  const uint32_t idleMs = uint32_t(settings.idleMinutes) * 60000;
  if (millis() - last > idleMs) enterSleep(screen != Screen::Message);
  // Boards that nap: woken on a page by something that wasn't a gesture (a brush
  // of the screen, a button too short to count): back to sleep after a few seconds.
  if (kNapBetweenPages && settings.deepSleep && screen == Screen::Reading &&
      pendingKey == Key::None && millis() - last > 3000) {
    nap();
  }
  delay(wifi ? 1 : 5);
}

}  // namespace app
