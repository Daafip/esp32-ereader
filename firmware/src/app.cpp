// The reader: wake handling, reading screen, menus, sleep.
//
// With "Sleep after page" on (default) every page turn is one wake-up: setup()
// classifies the button that woke the chip, shows the next/previous page and
// goes back to deep sleep with an idle timer. When that timer fires instead of
// a button, the sleep screen is shown and only a button wakes the reader.
// Menus run awake in loop() until a book page is shown again or the idle
// timeout hits. Light-sleep boards (M5) nap in loop() instead.
#include "app.h"

#include <Arduino.h>
#include <SD.h>
#include <esp_attr.h>
#include <string.h>
#include <time.h>

#include <vector>

#include "board.h"
#include "book.h"
#include "bookmarks.h"
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

const S kMainItems[] = {S::Resume,  S::Chapters,   S::Bookmarks,    S::GoTo,
                        S::Library, S::Statistics, S::WifiTransfer, S::Settings,
                        S::About,   S::Lock,       S::Sleep};
constexpr int kMainCount = sizeof(kMainItems) / sizeof(kMainItems[0]);
bool messageOffersWifi = false;  // "No books yet": MENU starts Wi-Fi transfer
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
  input::setFlipped(settings.flip);
  touch::setFlipped(settings.flip);
  display::setFlipped(settings.flip);  // a change makes the next refresh a full one
}

void setScreen(Screen s) {
  screen = s;
  rtc.screen = s;
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
  fs::File f = SD.open(storage::picturePath(shape, int(i)), FILE_READ);
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
  messageOffersWifi = false;
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

void showReading(Refresh requested) {
  // Big panels (T5-4.7") can't keep the shown image through deep sleep: draw
  // the page that is on the glass again, so this turn can still be partial.
  if (requested == Refresh::Partial && settings.wakeRefresh != WakeRefresh::Full &&
      !display::shownValid() && rtc.screen == Screen::Reading && rtc.page != page &&
      rtc.page < book.pageCount() && drawPage(rtc.page, rtc.pageBattery)) {
    display::assumeShown();
  }
  if (!drawPage(page, batteryPct)) {
    showMessage(ui::Icon::Card, tr(S::ReadError), baseName(book.path()), tr(S::CardRemoved));
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
  rtc.imageMs = millis();
  const display::Timing t = display::lastTiming();
  Serial.printf("page %u/%u chapter %d: %s refresh, data %u ms, refresh %u ms, at %u ms\n",
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

// Light-sleep boards (M5): sleep until a touch or button. The idle timer ends
// in the sleep screen and power-off. Back in loop(), the touch or press that
// woke the chip is read as usual and the panel still holds its image.
void nap() {
  display::sleep(true);
  input::waitAllReleased();
  if (!power::lightSleep(uint32_t(settings.idleMinutes) * 60)) enterSleep(true);
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
  if (kLightSleep) {
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
  const S id = kMainItems[i];
  strlcpy(it.label, tr(id), sizeof(it.label));
  it.chevron = id != S::Resume && id != S::Sleep && id != S::Lock;
  if (id == S::Resume) snprintf(it.detail, sizeof(it.detail), "%d%%", percentRead());
  if (id == S::Bookmarks && marks.count()) snprintf(it.detail, sizeof(it.detail), "%d", marks.count());
}

void renderMainMenu() {
  ui::menu(book.header().title, list, mainItem, nullptr, listNext(), menuHint(S::Open), batteryPct);
  display::show(menuRefresh());
}

void openMainMenu() {
  input::setRepeat(true);
  list.reset(kMainCount, ui::menuRows(), 0);
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
  ui::menu(title.c_str(), list, libraryItem, nullptr, listNext(), menuHint(S::Open), batteryPct);
  display::show(menuRefresh());
}

void showNoBooks() {
  // Any press starts Wi-Fi transfer: the only way forward without a PC (the card
  // is read again when it ends). A "retry" here just restarted to the same screen.
  ui::message(ui::Icon::Book, tr(S::NoBooksYet), tr(S::NoBooksLine1), tr(S::NoBooksLine2),
              {Button::Next, tr(S::WifiTransfer), nullptr}, kNoHint);
  display::show(Refresh::Full);
  setScreen(Screen::Message);
  messageOffersWifi = true;
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
  snprintf(v[2], sizeof(v[2]), "%s", book.header().title);
  snprintf(v[3], sizeof(v[3]), tr(S::PositionValue), unsigned(page + 1),
           unsigned(book.pageCount()), book.chapterForPage(page) + 1, book.tocCount());
  char card[16] = "-";
  const int64_t room = storage::freeBytes();
  if (room >= 0) formatBytes(uint64_t(room), card, sizeof(card));
  snprintf(v[4], sizeof(v[4]), tr(S::MemoryValue), card, unsigned(ESP.getFreeHeap() / 1024));
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

void stopTransfer() {
  transfer::stop();
  // The page may have changed settings, the lock, the statistics or the name.
  settings = prefs::loadSettings(boardDefaults());
  applySettings();
  locked = prefs::locked();
  rtc.stats = prefs::loadStats();
  rtc.statsUnsaved = 0;
  // Reopen from NVS, not the RTC page: the book may have been restyled to a new
  // page count, moved to a folder, or given another position from the page.
  const String last = prefs::lastBook();
  if (!(last.length() && openBook(last.c_str(), true)) &&
      !(rtc.bookPath[0] && openBook(rtc.bookPath, true))) {
    restoreBook();
  }
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

void mainMenuKey(Key k) {
  switch (listKey(k)) {
    case ListAction::Moved: renderMainMenu(); break;
    case ListAction::Back: resume(); break;
    case ListAction::Select:
      switch (kMainItems[list.selected()]) {
        case S::Chapters: openChapters(); break;
        case S::Bookmarks: openBookmarks(); break;
        case S::GoTo: openGoTo(); break;
        case S::Library: openLibrary(); break;
        case S::Statistics: openStatistics(); break;
        case S::WifiTransfer: startTransfer(); break;
        case S::Settings: openSettings(); break;
        case S::About: openAbout(); break;
        case S::Lock:
          setLocked(true);
          resume();
          break;
        case S::Sleep: enterSleep(true);
        default: resume(); break;
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
      } else if (book.isOpen()) {
        openMainMenu();
      } else {
        renderLibrary();  // nothing to go back to
      }
      break;
    case ListAction::Select: {
      const storage::BookEntry e = library[list.selected()];
      if (e.isFolder) {
        openLibrary(e.title);
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
      renderSettings();
      break;
    case ListAction::None: break;
  }
}

bool isList(Screen s) {
  return s == Screen::MainMenu || s == Screen::Chapters || s == Screen::Bookmarks ||
         s == Screen::Library || s == Screen::SettingsMenu;
}

void renderList() {
  switch (screen) {
    case Screen::MainMenu: renderMainMenu(); break;
    case Screen::Chapters: renderChapters(); break;
    case Screen::Bookmarks: renderBookmarks(); break;
    case Screen::Library: renderLibrary(); break;
    case Screen::SettingsMenu: renderSettings(); break;
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
        switch (readingTap(t.x, t.y, kPageW, kPageH)) {
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
    case Screen::Message:
      if (messageOffersWifi) {  // "No books yet"
        startTransfer();
      } else {
        ESP.restart();  // retry from the start (card inserted, book copied)
      }
      break;
    case Screen::Sleeping:
    case Screen::LowBattery:
    case Screen::None:
      if (locked && k != Key::MenuLong) break;  // locked: only the unlock gesture
      if (locked) setLocked(false);
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
  power::begin();  // M5Paper: hold the power on before anything else
  Serial.begin(115200);
  const power::Wake wake = power::wakeReason();
  prefs::begin();
  settings = prefs::loadSettings(boardDefaults());
  setLanguage(settings.language);
  locked = prefs::locked();
  input::begin(settings.longPressMs);
  rtcclock::restore();  // M5 boards: the clock after switching off
  Key key = Key::None;
  if (wake == power::Wake::NextButton) key = input::wakePress(kBtnNext);
  if (wake == power::Wake::MenuButton) key = input::wakePress(kBtnMenu);

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
  const bool buttonWake = wake == power::Wake::NextButton || wake == power::Wake::MenuButton;
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
    showMessage(ui::Icon::Card, tr(S::NoCard), tr(S::NoCardLine1), tr(S::NoCardLine2));
    return;
  }
  if (!restoreBook()) {
    // first start, or the last book is gone. A press that woke "No books yet"
    // asks for Wi-Fi transfer (it's the only thing that screen offers).
    if (key != Key::None && shown == Screen::Message) {
      std::vector<storage::BookEntry> any;
      const storage::Status status = storage::listAllBooks(any, 1);
      if (status == storage::Status::NoBooks || status == storage::Status::NoBooksDir) {
        startTransfer();
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
  handle(input::poll());
  uint32_t last = input::lastActivityMs();
  if (wifi && int32_t(transfer::info().lastActivityMs - last) > 0) last = transfer::info().lastActivityMs;
  const uint32_t idleMs = uint32_t(settings.idleMinutes) * 60000;
  if (millis() - last > idleMs) enterSleep(screen != Screen::Message);
  // Light-sleep boards: woken on a page by something that wasn't a gesture
  // (a brush of the screen): back to sleep after a few seconds.
  if (kLightSleep && settings.deepSleep && screen == Screen::Reading && millis() - last > 3000) {
    nap();
  }
  delay(wifi ? 1 : 5);
}

}  // namespace app
