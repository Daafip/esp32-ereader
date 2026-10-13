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
  GoTo,
  Library,
  SettingsMenu,
  About,
  WifiTransfer,
};

// What the panel shows and where we are, kept through deep sleep (RTC RAM).
constexpr uint32_t kRtcMagic = 0x54354232;
struct RtcState {
  uint32_t magic;
  char bookPath[128];
  uint32_t page;
  int32_t chapter;
  Screen screen;
  int8_t pageBattery;  // battery % in the status bar of the page shown
  uint32_t wakes;
  uint32_t imageMs;  // app start -> image refreshed, last time
};
RTC_DATA_ATTR RtcState rtc;

constexpr uint32_t kNoBatteryMv = 2500;  // below this the ADC sees no battery (USB only)
// Footer hints: button role, short-press action, long-press action.
using ui::Button;
const ui::Hint kNoHint = {Button::None, nullptr, nullptr};
const ui::Hint kListNext = {Button::Next, ui::kArrowDown, ui::kArrowUp};
const ui::Hint kListOpen = {Button::Menu, "open", "back"};
const ui::Hint kListGo = {Button::Menu, "go", "back"};
const ui::Hint kListChange = {Button::Menu, "edit", "back"};
const ui::Hint kBack = {Button::Menu, "back", nullptr};

Settings settings;
Book book;
uint32_t page = 0;
uint32_t batteryMv = 0;
int batteryPct = -1;
Screen screen = Screen::None;
ListModel list;
std::vector<storage::BookEntry> library;
int goToPct = 0;

const char *const kMainItems[] = {"Resume",   "Chapters", "Go to position", "Library",
                                  "Wi-Fi transfer", "Settings", "About",  "Sleep"};
enum MainItem {
  kResume,
  kChapters,
  kGoTo,
  kLibrary,
  kWifi,
  kSettings,
  kAbout,
  kSleep,
  kMainCount
};
bool messageOffersWifi = false;  // "No books yet": MENU starts Wi-Fi transfer
int wifiStep = 0;                // QR shown: 0 = automatic, 1 = join Wi-Fi, 2 = open page

void setScreen(Screen s) {
  screen = s;
  rtc.screen = s;
}

// --- sleep -------------------------------------------------------------------

int percentRead() {
  if (!book.isOpen()) return -1;
  return book.pageCount() > 1 ? int(uint64_t(page) * 100 / (book.pageCount() - 1)) : 100;
}

[[noreturn]] void enterSleep(bool withSleepScreen) {
  transfer::stop();
  if (withSleepScreen) {
    ui::sleepScreen(book.isOpen() ? book.header().title : nullptr,
                    book.isOpen() ? book.header().author : nullptr, percentRead(), batteryPct);
    display::show(Refresh::Full);
    setScreen(Screen::Sleeping);
  }
  const bool reading = rtc.screen == Screen::Reading;
  display::sleep(reading && settings.wakeRefresh == WakeRefresh::Retain);
  input::waitAllReleased();
  // The idle timer only matters while a page is shown: it brings up the sleep screen.
  power::deepSleep(reading ? uint32_t(settings.idleMinutes) * 60 : 0);
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
    if (!quiet) showMessage(ui::Icon::Warning, "Cannot open book", baseName(path), book.error());
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

// Book page n with its status bar into the framebuffer.
bool drawPage(uint32_t n, int battery) {
  if (!book.readPage(n, display::framebuffer())) return false;
  const int chapter = book.chapterForPage(n);
  t5b::TocEntry entry;
  const char *title = chapter >= 0 && book.tocEntry(chapter, entry) ? entry.title : book.header().title;
  ui::statusBar(title, n, book.pageCount(), battery);
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
    showMessage(ui::Icon::Card, "Read error", baseName(book.path()), "Was the card removed?");
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
  ui::message(ui::Icon::Battery, "Battery low", "Charge the reader,", "then press a button", nullptr);
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

// --- menus -------------------------------------------------------------------

Refresh menuRefresh() {
  return decideRefresh(true, false, display::shownValid(), display::partialsSinceFull(),
                       settings.fullEvery);
}

void mainItem(int i, ui::Item &it, void *) {
  strlcpy(it.label, kMainItems[i], sizeof(it.label));
  it.chevron = i != kResume && i != kSleep;
  if (i == kResume) snprintf(it.detail, sizeof(it.detail), "%d%%", percentRead());
}

void renderMainMenu() {
  ui::menu(book.header().title, list, mainItem, nullptr, kListNext, kListOpen, batteryPct);
  display::show(menuRefresh());
}

void openMainMenu() {
  input::setRepeat(true);
  list.reset(kMainCount, ui::menuRows(), kResume);
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
  ui::menu("Chapters", list, chapterItem, nullptr, kListNext, kListGo, batteryPct);
  display::show(menuRefresh());
}

void openChapters() {
  const int current = book.chapterForPage(page);
  list.reset(book.tocCount(), ui::menuRows(), current < 0 ? 0 : current);
  setScreen(Screen::Chapters);
  renderChapters();
}

uint32_t pctToPage(int pct) { return uint32_t(uint64_t(book.pageCount() - 1) * pct / 100); }

void renderGoTo() {
  char value[8];
  char caption[40];
  snprintf(value, sizeof(value), "%d%%", goToPct);
  snprintf(caption, sizeof(caption), "Page %u of %u", unsigned(pctToPage(goToPct) + 1),
           unsigned(book.pageCount()));
  ui::valueEditor("Go to position", value, caption, goToPct, {Button::Next, "+1", "+10"},
                  kListGo, batteryPct);
  display::show(menuRefresh());
}

void openGoTo() {
  goToPct = percentRead();
  setScreen(Screen::GoTo);
  renderGoTo();
}

void libraryItem(int i, ui::Item &it, void *) {
  const storage::BookEntry &e = library[i];
  strlcpy(it.label, e.title.c_str(), sizeof(it.label));
  it.current = book.isOpen() && e.path == book.path();
  const uint32_t mark = it.current ? page : prefs::bookmark(e.id);
  if (mark == 0) {
    strlcpy(it.detail, "new", sizeof(it.detail));
  } else {
    const uint32_t last = e.pages > 1 ? e.pages - 1 : 1;
    snprintf(it.detail, sizeof(it.detail), "%u%%", unsigned(uint64_t(mark) * 100 / last));
  }
}

void renderLibrary() {
  ui::menu("Library", list, libraryItem, nullptr, kListNext, kListOpen, batteryPct);
  display::show(menuRefresh());
}

void showNoBooks() {
  ui::message(ui::Icon::Book, "No books yet", "Upload them over Wi-Fi, or copy", ".t5b files to /books on the card",
              {Button::Menu, "Wi-Fi transfer", nullptr}, {Button::Next, "retry", nullptr});
  display::show(Refresh::Full);
  setScreen(Screen::Message);
  messageOffersWifi = true;
}

void openLibrary() {
  const storage::Status status = storage::listBooks(library);
  if (status != storage::Status::Ok) {
    showNoBooks();
    return;
  }
  int selected = 0;
  for (size_t i = 0; i < library.size(); i++) {
    if (book.isOpen() && library[i].path == book.path()) selected = int(i);
  }
  input::setRepeat(true);
  list.reset(int(library.size()), ui::menuRows(), selected);
  setScreen(Screen::Library);
  renderLibrary();
}

void settingItem(int i, ui::Item &it, void *) {
  strlcpy(it.label, settingLabel(SettingId(i)), sizeof(it.label));
  settingValue(settings, SettingId(i), it.detail, sizeof(it.detail));
}

void renderSettings() {
  ui::menu("Settings", list, settingItem, nullptr, kListNext, kListChange, batteryPct);
  display::show(menuRefresh());
}

void openSettings() {
  list.reset(kSettingCount, ui::menuRows(), 0);
  setScreen(Screen::SettingsMenu);
  renderSettings();
}

void openAbout() {
  char v[6][72];
  snprintf(v[0], sizeof(v[0]), "%s", FW_VERSION);
  if (batteryPct >= 0) {
    snprintf(v[1], sizeof(v[1]), "%u.%02u V  \xC2\xB7  %d%%", unsigned(batteryMv / 1000),
             unsigned(batteryMv % 1000 / 10), batteryPct);
  } else {
    snprintf(v[1], sizeof(v[1]), "USB power");
  }
  snprintf(v[2], sizeof(v[2]), "%s", book.header().title);
  snprintf(v[3], sizeof(v[3]), "page %u/%u  \xC2\xB7  ch. %d/%d", unsigned(page + 1),
           unsigned(book.pageCount()), book.chapterForPage(page) + 1, book.tocCount());
  snprintf(v[4], sizeof(v[4]), "%u MB card  \xC2\xB7  %u KB free", unsigned(storage::cardSizeMb()),
           unsigned(ESP.getFreeHeap() / 1024));
  snprintf(v[5], sizeof(v[5]), "%u ms  \xC2\xB7  %u wakes", unsigned(rtc.imageMs),
           unsigned(rtc.wakes));
  const char *labels[] = {"Firmware", "Battery", "Book", "Position", "Memory", "Wake to page"};
  const char *values[] = {v[0], v[1], v[2], v[3], v[4], v[5]};
  ui::details("About", labels, values, 6, kNoHint, kBack, batteryPct);
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
    v = {qr, 1, "Join the Wi-Fi", "Network", t.ssid, "Password", t.password, t.event};
  } else {
    strlcpy(qr, t.url, sizeof(qr));
    v = {qr, step, "Open the page", "Address", t.address, "Code", t.code, t.event};
  }
  const ui::Hint other = {Button::Next, step == 1 ? "show page" : "show Wi-Fi", nullptr};
  ui::wifiScreen(v, step ? other : kNoHint, {Button::Menu, "done", nullptr}, batteryPct);
  display::show(menuRefresh());
}

void startTransfer() {
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
  // Reopen from NVS, not the RTC page: the book may have been restyled to a new
  // page count, and the upload moved its bookmark accordingly.
  if (!(rtc.bookPath[0] && openBook(rtc.bookPath, true))) restoreBook();
  openLibrary();  // shows what is on the card now
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
        case kWifi: startTransfer(); break;
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
    case Key::None:
    case Key::Touch: break;
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

bool isList(Screen s) {
  return s == Screen::MainMenu || s == Screen::Chapters || s == Screen::Library ||
         s == Screen::SettingsMenu;
}

void renderList() {
  switch (screen) {
    case Screen::MainMenu: renderMainMenu(); break;
    case Screen::Chapters: renderChapters(); break;
    case Screen::Library: renderLibrary(); break;
    case Screen::SettingsMenu: renderSettings(); break;
    default: break;
  }
}

// Touch boards: what a gesture means on the current screen. Reading: tap zones
// (left third back, top strip menu, rest next), swipes, long press = menu.
// Elsewhere: rows and footer buttons as drawn (ui::hit), swipe up/down pages
// through a list, long press = back, a tap anywhere on a message = any key.
Key touchKey(const TouchEvent &t) {
  if (screen == Screen::Reading) {
    switch (t.kind) {
      case TouchKind::SwipeLeft: return Key::NextShort;
      case TouchKind::SwipeRight: return Key::NextLong;
      case TouchKind::LongPress: return Key::MenuShort;
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
                      screen == Screen::Sleeping || screen == Screen::LowBattery ||
                      screen == Screen::None;
  return anyKey ? Key::NextShort : Key::None;
}

void handle(Key k) {
  if (k == Key::Touch) k = touchKey(input::lastTouch());
  if (k == Key::None) return;
  switch (screen) {
    case Screen::Reading: readingKey(k); break;
    case Screen::MainMenu: mainMenuKey(k); break;
    case Screen::Chapters: chaptersKey(k); break;
    case Screen::GoTo: goToKey(k); break;
    case Screen::Library: libraryKey(k); break;
    case Screen::SettingsMenu: settingsKey(k); break;
    case Screen::About: openMainMenu(); break;
    case Screen::WifiTransfer: transferKey(k); break;
    case Screen::Message:
      if (messageOffersWifi && k == Key::MenuShort) {
        startTransfer();
      } else {
        ESP.restart();  // retry from the start (card inserted, book copied)
      }
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
  power::begin();  // M5Paper: hold the power on before anything else
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
  readBattery();
  Serial.printf("\nT5 reader %s: wake by %s, key %d, battery %u mV, shown screen %d\n", FW_VERSION,
                power::wakeName(wake), int(key), unsigned(batteryMv), int(shown));

  display::begin();
  if (batteryMv > kNoBatteryMv && batteryMv < kBatteryLowMv) {
    if (shown != Screen::LowBattery) showLowBattery();
    display::sleep(false);
    power::deepSleep(0);
  }
  if (!storage::mount()) {
    showMessage(ui::Icon::Card, "No SD card", "Insert a FAT32 card", "with books in /books");
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
