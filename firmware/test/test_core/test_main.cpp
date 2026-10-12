// Host unit tests for lib/core: `pixi run fw-test`.
#include <string.h>
#include <unity.h>

#include "bitmap.h"
#include "button.h"
#include "golden_header.h"
#include "menu.h"
#include "policy.h"
#include "settings.h"
#include "t5b_format.h"
#include "text_util.h"
#include "web_util.h"

void setUp() {}
void tearDown() {}

// --- t5b_format -------------------------------------------------------------

static void test_parse_golden_header_from_python_writer() {
  t5b::Header h;
  TEST_ASSERT_EQUAL(int(t5b::Error::None), int(t5b::parseHeader(kGoldenHeader, 146, h)));
  TEST_ASSERT_EQUAL(250, h.pageW);
  TEST_ASSERT_EQUAL(122, h.pageH);
  TEST_ASSERT_EQUAL(32, h.stride);
  TEST_ASSERT_EQUAL(3, h.pageCount);
  TEST_ASSERT_EQUAL(2, h.tocCount);
  TEST_ASSERT_EQUAL_STRING("Max Havelaar \xE2\x80\x94 \xC3\xA9\xC3\xA9n", h.title);
  TEST_ASSERT_EQUAL_STRING("Multatuli", h.author);
  TEST_ASSERT_EQUAL(3904, h.pageBytes());
  TEST_ASSERT_EQUAL(146 + 2 * 52, h.pageOffset(0));
  TEST_ASSERT_EQUAL(146 + 2 * 52 + 2 * 3904, h.pageOffset(2));

  t5b::TocEntry e;
  t5b::parseTocEntry(kGoldenHeader + 146, e);
  TEST_ASSERT_EQUAL(0, e.startPage);
  TEST_ASSERT_EQUAL_STRING("Begin", e.title);
  t5b::parseTocEntry(kGoldenHeader + 146 + 52, e);
  TEST_ASSERT_EQUAL(2, e.startPage);
  TEST_ASSERT_EQUAL_STRING("Hoofdstuk twee", e.title);
}

static void test_header_errors() {
  uint8_t d[146];
  memcpy(d, kGoldenHeader, sizeof(d));
  t5b::Header h;
  TEST_ASSERT_EQUAL(int(t5b::Error::TooShort), int(t5b::parseHeader(d, 100, h)));
  d[0] = 'X';
  TEST_ASSERT_EQUAL(int(t5b::Error::BadMagic), int(t5b::parseHeader(d, 146, h)));
  d[0] = 'T';
  d[4] = 2;
  TEST_ASSERT_EQUAL(int(t5b::Error::BadVersion), int(t5b::parseHeader(d, 146, h)));
  d[4] = 1;
  d[10] = 31;
  TEST_ASSERT_EQUAL(int(t5b::Error::BadStride), int(t5b::parseHeader(d, 146, h)));
}

static void test_title_without_terminator_is_terminated() {
  uint8_t d[146];
  memcpy(d, kGoldenHeader, sizeof(d));
  memset(d + 18, 'A', 64);
  t5b::Header h;
  t5b::parseHeader(d, 146, h);
  TEST_ASSERT_EQUAL(64, strlen(h.title));
}

static void test_validate() {
  t5b::Header h;
  t5b::parseHeader(kGoldenHeader, 146, h);
  const uint32_t size = 146 + 2 * 52 + 3 * 3904;
  TEST_ASSERT_EQUAL(int(t5b::Error::None), int(t5b::validate(h, 250, 122, size)));
  TEST_ASSERT_EQUAL(int(t5b::Error::SizeMismatch), int(t5b::validate(h, 250, 122, size - 1)));
  TEST_ASSERT_EQUAL(int(t5b::Error::WrongGeometry), int(t5b::validate(h, 212, 104, size)));
  h.pageCount = 0;
  TEST_ASSERT_EQUAL(int(t5b::Error::NoPages), int(t5b::validate(h, 250, 122, 250)));
}

static void test_chapter_for_page() {
  const uint32_t starts[] = {0, 10, 25};
  TEST_ASSERT_EQUAL(0, t5b::chapterForPage(starts, 3, 0));
  TEST_ASSERT_EQUAL(0, t5b::chapterForPage(starts, 3, 9));
  TEST_ASSERT_EQUAL(1, t5b::chapterForPage(starts, 3, 10));
  TEST_ASSERT_EQUAL(2, t5b::chapterForPage(starts, 3, 3000));
  const uint32_t late[] = {5, 10};
  TEST_ASSERT_EQUAL(-1, t5b::chapterForPage(late, 2, 2));
  TEST_ASSERT_EQUAL(-1, t5b::chapterForPage(late, 0, 7));
}

static void test_book_id_and_key() {
  const uint32_t a = t5b::bookId("Max Havelaar", 3596);
  TEST_ASSERT_EQUAL_UINT32(a, t5b::bookId("Max Havelaar", 3596));
  TEST_ASSERT_NOT_EQUAL(a, t5b::bookId("Max Havelaar", 3597));
  TEST_ASSERT_NOT_EQUAL(a, t5b::bookId("Max Havelaa", 3596));
  char key[10];
  t5b::bookmarkKey(0x0012abcd, key);
  TEST_ASSERT_EQUAL_STRING("b0012abcd", key);
}

// --- button -----------------------------------------------------------------

static void test_short_press_with_bounce() {
  ButtonTracker b;  // debounce 25, long 600
  TEST_ASSERT_EQUAL(int(ButtonEvent::None), int(b.update(true, 0)));
  TEST_ASSERT_EQUAL(int(ButtonEvent::None), int(b.update(false, 5)));  // bounce
  TEST_ASSERT_EQUAL(int(ButtonEvent::None), int(b.update(true, 8)));
  TEST_ASSERT_EQUAL(int(ButtonEvent::None), int(b.update(true, 40)));
  TEST_ASSERT_TRUE(b.isDown());
  TEST_ASSERT_EQUAL(int(ButtonEvent::None), int(b.update(false, 150)));
  TEST_ASSERT_EQUAL(int(ButtonEvent::Short), int(b.update(false, 180)));
  TEST_ASSERT_FALSE(b.isDown());
  TEST_ASSERT_EQUAL(int(ButtonEvent::None), int(b.update(false, 400)));
}

static void test_long_press_fires_while_held_and_not_on_release() {
  ButtonTracker b;
  b.update(true, 0);
  b.update(true, 30);
  TEST_ASSERT_EQUAL(int(ButtonEvent::None), int(b.update(true, 599)));
  TEST_ASSERT_EQUAL(int(ButtonEvent::Long), int(b.update(true, 600)));
  TEST_ASSERT_EQUAL(int(ButtonEvent::None), int(b.update(true, 2000)));  // no repeat configured
  b.update(false, 2100);
  TEST_ASSERT_EQUAL(int(ButtonEvent::None), int(b.update(false, 2200)));
}

static void test_repeat() {
  ButtonTracker b({25, 600, 200});
  b.update(true, 0);
  b.update(true, 30);
  TEST_ASSERT_EQUAL(int(ButtonEvent::Long), int(b.update(true, 600)));
  TEST_ASSERT_EQUAL(int(ButtonEvent::None), int(b.update(true, 799)));
  TEST_ASSERT_EQUAL(int(ButtonEvent::Repeat), int(b.update(true, 800)));
  TEST_ASSERT_EQUAL(int(ButtonEvent::Repeat), int(b.update(true, 1000)));
}

static void test_start_pressed_after_wake() {
  ButtonTracker b;
  b.startPressed(0);
  TEST_ASSERT_TRUE(b.isDown());
  TEST_ASSERT_EQUAL(int(ButtonEvent::Long), int(b.update(true, 650)));

  ButtonTracker quick;
  quick.startPressed(0);
  quick.update(false, 200);
  TEST_ASSERT_EQUAL(int(ButtonEvent::Short), int(quick.update(false, 230)));
}

// --- one-button gestures ------------------------------------------------------------

// Raw level `pressed` from `from` to `to` (exclusive), 5 ms steps; returns the
// first gesture seen (and the time in *at).
static Gesture feed(ClickDecoder &d, bool pressed, uint32_t from, uint32_t to,
                    uint32_t *at = nullptr) {
  for (uint32_t t = from; t < to; t += 5) {
    const Gesture g = d.update(pressed, t);
    if (g != Gesture::None) {
      if (at) *at = t;
      return g;
    }
  }
  return Gesture::None;
}

static void test_tap_waits_for_the_double_tap_window() {
  ClickDecoder d;  // debounce 25, hold 600, long hold 1600, double tap 300
  uint32_t at = 0;
  TEST_ASSERT_EQUAL(int(Gesture::None), int(feed(d, true, 0, 100)));
  TEST_ASSERT_EQUAL(int(Gesture::Tap), int(feed(d, false, 100, 1000, &at)));
  TEST_ASSERT_EQUAL(400, at);  // released at 100 + 300 ms window
  TEST_ASSERT_FALSE(d.busy());
}

static void test_double_tap() {
  ClickDecoder d;
  uint32_t at = 0;
  feed(d, true, 0, 100);
  TEST_ASSERT_EQUAL(int(Gesture::None), int(feed(d, false, 100, 250)));
  TEST_ASSERT_TRUE(d.busy());
  TEST_ASSERT_EQUAL(int(Gesture::None), int(feed(d, true, 250, 350)));
  TEST_ASSERT_EQUAL(int(Gesture::DoubleTap), int(feed(d, false, 350, 1000, &at)));
  TEST_ASSERT_EQUAL(375, at);
  TEST_ASSERT_EQUAL(int(Gesture::None), int(feed(d, false, 1000, 2000)));  // no extra tap
}

static void test_hold_and_long_hold() {
  ClickDecoder d;
  uint32_t at = 0;
  TEST_ASSERT_EQUAL(int(Gesture::None), int(feed(d, true, 0, 800)));  // nothing while held
  TEST_ASSERT_EQUAL(int(Gesture::Hold), int(feed(d, false, 800, 1000, &at)));
  TEST_ASSERT_EQUAL(825, at);

  ClickDecoder l;
  TEST_ASSERT_EQUAL(int(Gesture::LongHold), int(feed(l, true, 0, 3000, &at)));
  TEST_ASSERT_EQUAL(1600, at);
  TEST_ASSERT_EQUAL(int(Gesture::None), int(feed(l, true, 1605, 3000)));
  TEST_ASSERT_EQUAL(int(Gesture::None), int(feed(l, false, 3000, 4000)));  // release: nothing
}

static void test_tap_then_hold_is_not_a_double_tap() {
  ClickDecoder d;
  uint32_t at = 0;
  feed(d, true, 0, 100);
  feed(d, false, 100, 200);
  TEST_ASSERT_EQUAL(int(Gesture::Tap), int(feed(d, true, 200, 1000, &at)));
  TEST_ASSERT_EQUAL(800, at);  // second press reached the hold time: first tap reported
  TEST_ASSERT_EQUAL(int(Gesture::None), int(feed(d, true, 805, 1000)));
  TEST_ASSERT_EQUAL(int(Gesture::Hold), int(feed(d, false, 1000, 1100)));
}

static void test_bounce_is_one_tap() {
  ClickDecoder d;
  // contact bounce on press and release (shorter than the 25 ms debounce)
  const bool levels[] = {true, false, true, false, true};
  uint32_t t = 0;
  for (bool l : levels) TEST_ASSERT_EQUAL(int(Gesture::None), int(d.update(l, t += 4)));
  feed(d, true, t, 120);
  d.update(false, 120);
  d.update(true, 124);
  TEST_ASSERT_EQUAL(int(Gesture::Tap), int(feed(d, false, 128, 1000)));
  TEST_ASSERT_EQUAL(int(Gesture::None), int(feed(d, false, 1000, 2000)));
}

static void test_click_decoder_after_wake() {
  ClickDecoder d;
  uint32_t at = 0;
  d.startPressed(0);
  TEST_ASSERT_TRUE(d.isDown());
  TEST_ASSERT_EQUAL(int(Gesture::Tap), int(feed(d, false, 180, 1000, &at)));
  TEST_ASSERT_EQUAL(480, at);

  ClickDecoder h;
  h.startPressed(0);
  TEST_ASSERT_EQUAL(int(Gesture::Hold), int(feed(h, false, 700, 1000)));
}

// --- bitmap -------------------------------------------------------------------------

static bool pixel(const uint8_t *b, int w, int x, int y) {  // true = black
  return !(b[y * ((w + 7) / 8) + (x >> 3)] & (0x80 >> (x & 7)));
}

static void test_rotate_1bpp() {
  // 10 x 3 canvas (stride 2), white, with black pixels at (0,0), (9,0) and (3,2)
  uint8_t src[2 * 3];
  memset(src, 0xFF, sizeof(src));
  src[0] &= ~0x80;
  src[1] &= ~0x40;
  src[2 * 2] &= ~0x10;
  uint8_t r90[1 * 10];
  rotate1bpp(src, 10, 3, r90, 90);  // panel 3 x 10: (x, y) -> (y, 9 - x)
  int black = 0;
  for (int y = 0; y < 10; y++) {
    for (int x = 0; x < 3; x++) black += pixel(r90, 3, x, y);
  }
  TEST_ASSERT_EQUAL(3, black);
  TEST_ASSERT_TRUE(pixel(r90, 3, 0, 9));
  TEST_ASSERT_TRUE(pixel(r90, 3, 0, 0));
  TEST_ASSERT_TRUE(pixel(r90, 3, 2, 6));
  TEST_ASSERT_EQUAL_HEX8(0xFF, r90[0] | 0x80);  // padding bits stay white

  uint8_t r270[1 * 10];
  rotate1bpp(src, 10, 3, r270, 270);  // (x, y) -> (2 - y, x)
  TEST_ASSERT_TRUE(pixel(r270, 3, 2, 0));
  TEST_ASSERT_TRUE(pixel(r270, 3, 2, 9));
  TEST_ASSERT_TRUE(pixel(r270, 3, 0, 3));

  // 90 then 270 (on the 3 x 10 result) restores the original
  uint8_t back[2 * 3];
  rotate1bpp(r90, 3, 10, back, 270);
  TEST_ASSERT_EQUAL_MEMORY(src, back, sizeof(src));
}

// --- policy -----------------------------------------------------------------

static void test_refresh_policy() {
  TEST_ASSERT_EQUAL(int(Refresh::Partial), int(decideRefresh(true, false, true, 0, 10)));
  TEST_ASSERT_EQUAL(int(Refresh::Partial), int(decideRefresh(true, false, true, 9, 10)));
  TEST_ASSERT_EQUAL(int(Refresh::Full), int(decideRefresh(true, false, true, 10, 10)));
  TEST_ASSERT_EQUAL(int(Refresh::Full), int(decideRefresh(true, true, true, 0, 10)));
  TEST_ASSERT_EQUAL(int(Refresh::Full), int(decideRefresh(true, false, false, 0, 10)));
  TEST_ASSERT_EQUAL(int(Refresh::Full), int(decideRefresh(false, false, true, 0, 10)));
  TEST_ASSERT_EQUAL(int(Refresh::Partial), int(decideRefresh(true, false, true, 500, 0)));
}

static void test_battery_percent() {
  TEST_ASSERT_EQUAL(100, batteryPercent(4200));
  TEST_ASSERT_EQUAL(100, batteryPercent(4350));
  TEST_ASSERT_EQUAL(0, batteryPercent(3300));
  TEST_ASSERT_EQUAL(0, batteryPercent(2900));
  int last = -1;
  for (uint32_t mv = 3300; mv <= 4200; mv += 10) {
    const int pct = batteryPercent(mv);
    TEST_ASSERT_TRUE(pct >= last);
    last = pct;
  }
}

// --- text_util --------------------------------------------------------------

static void test_utf8_decode() {
  uint32_t cp;
  TEST_ASSERT_EQUAL(1, utf8Decode("A", cp));
  TEST_ASSERT_EQUAL_HEX32(0x41, cp);
  TEST_ASSERT_EQUAL(2, utf8Decode("\xC3\xA9", cp));
  TEST_ASSERT_EQUAL_HEX32(0xE9, cp);
  TEST_ASSERT_EQUAL(3, utf8Decode("\xE2\x82\xAC", cp));
  TEST_ASSERT_EQUAL_HEX32(0x20AC, cp);
  TEST_ASSERT_EQUAL(1, utf8Decode("\xFF", cp));
  TEST_ASSERT_EQUAL_HEX32(0xFFFD, cp);
  TEST_ASSERT_EQUAL(1, utf8Decode("\xC3", cp));  // truncated at the terminator
  TEST_ASSERT_EQUAL_HEX32(0xFFFD, cp);
}

static void test_ascii_fold() {
  char out[64];
  asciiFold("Caf\xC3\xA9 \xE2\x80\x9Cqu\xC3\xABte\xE2\x80\x9D \xE2\x80\x94 \xE2\x80\xA6 \xE2\x80\x99s \xE6\xBC\xA2",
            out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Cafe \"quete\" - ... 's ?", out);
  asciiFold("Caf\xC3\xA9 au lait", out, 5);
  TEST_ASSERT_EQUAL_STRING("Cafe", out);
  asciiFold("\xC3\x86sop \xC3\x9F", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("AEsop ss", out);
}

static int measure6(const char *s, void *) {
  int n = 0;
  while (*s) {
    uint32_t cp;
    s += utf8Decode(s, cp);
    n++;
  }
  return 6 * n;
}

static void test_fit_text() {
  char out[64];
  const char *ell = "\xE2\x80\xA6";
  TEST_ASSERT_FALSE(fitText("kort", 60, measure6, nullptr, ell, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("kort", out);
  TEST_ASSERT_TRUE(fitText("Hoofdstuk \xC3\xA9\xC3\xA9n", 60, measure6, nullptr, ell, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("Hoofdstuk\xE2\x80\xA6", out);
  TEST_ASSERT_TRUE(fitText("Hoofdstuk \xC3\xA9\xC3\xA9n", 72, measure6, nullptr, ell, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("Hoofdstuk \xC3\xA9\xE2\x80\xA6", out);  // é kept whole
  TEST_ASSERT_TRUE(fitText("abc def", 30, measure6, nullptr, ell, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("abc\xE2\x80\xA6", out);  // no space before the ellipsis
  TEST_ASSERT_TRUE(fitText("abcdef", 3, measure6, nullptr, ell, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("", out);
  TEST_ASSERT_TRUE(fitText("abcdefghijkl", 600, measure6, nullptr, "...", out, 8));
  TEST_ASSERT_EQUAL_STRING("abcd...", out);  // limited by the buffer, not the width
}

static void test_format_progress() {
  char out[32];
  formatProgress(0, 3596, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("1/3596  \xC2\xB7  0%", out);
  formatProgress(3595, 3596, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("3596/3596  \xC2\xB7  100%", out);
}

// --- menu -------------------------------------------------------------------

static void test_list_model_scrolls_and_wraps() {
  ListModel m;
  m.reset(10, 4);
  for (int i = 0; i < 5; i++) m.next();
  TEST_ASSERT_EQUAL(5, m.selected());
  TEST_ASSERT_EQUAL(2, m.first());
  TEST_ASSERT_TRUE(m.canScrollUp());
  TEST_ASSERT_TRUE(m.canScrollDown());
  m.reset(10, 4);
  m.prev();
  TEST_ASSERT_EQUAL(9, m.selected());
  TEST_ASSERT_EQUAL(6, m.first());
  TEST_ASSERT_FALSE(m.canScrollDown());
  m.next();
  TEST_ASSERT_EQUAL(0, m.selected());
  TEST_ASSERT_EQUAL(0, m.first());
  m.reset(10, 4, 8);
  TEST_ASSERT_EQUAL(5, m.first());  // selection lands on the bottom row
  m.reset(2, 4);
  TEST_ASSERT_FALSE(m.canScrollDown());
  m.reset(0, 4);
  m.next();
  TEST_ASSERT_EQUAL(0, m.selected());
}

// --- settings ---------------------------------------------------------------

static void test_settings_cycle_value_sanitize() {
  Settings s;
  char text[48];
  TEST_ASSERT_EQUAL_STRING("Full refresh", settingLabel(SettingId::FullEvery));
  settingValue(s, SettingId::FullEvery, text, sizeof(text));
  TEST_ASSERT_EQUAL_STRING("every 10", text);
  cycleSetting(s, SettingId::FullEvery);
  TEST_ASSERT_EQUAL(15, s.fullEvery);
  for (int i = 0; i < 3; i++) cycleSetting(s, SettingId::FullEvery);
  TEST_ASSERT_EQUAL(0, s.fullEvery);
  settingValue(s, SettingId::FullEvery, text, sizeof(text));
  TEST_ASSERT_EQUAL_STRING("chapters only", text);
  cycleSetting(s, SettingId::FullEvery);
  TEST_ASSERT_EQUAL(5, s.fullEvery);

  cycleSetting(s, SettingId::WakeRefresh);
  TEST_ASSERT_EQUAL(int(WakeRefresh::Retain), int(s.wakeRefresh));
  settingValue(s, SettingId::WakeRefresh, text, sizeof(text));
  TEST_ASSERT_EQUAL_STRING("retain", text);
  cycleSetting(s, SettingId::DeepSleep);
  settingValue(s, SettingId::DeepSleep, text, sizeof(text));
  TEST_ASSERT_EQUAL_STRING("off", text);
  settingValue(s, SettingId::LongPress, text, sizeof(text));
  TEST_ASSERT_EQUAL_STRING("600 ms", text);

  s.fullEvery = 7;
  s.longPressMs = 1;
  s.idleMinutes = 0;
  s.wakeRefresh = WakeRefresh(9);
  sanitizeSettings(s);
  const Settings d;
  TEST_ASSERT_EQUAL(d.fullEvery, s.fullEvery);
  TEST_ASSERT_EQUAL(d.longPressMs, s.longPressMs);
  TEST_ASSERT_EQUAL(d.idleMinutes, s.idleMinutes);
  TEST_ASSERT_EQUAL(int(d.wakeRefresh), int(s.wakeRefresh));
}

static void test_wrap_text() {
  char lines[3][32];
  const char *ell = "...";
  // 10 code points per line at 6 px each
  int n = wrapText("Max Havelaar of de koffiveilingen", 60, measure6, nullptr, ell, lines[0],
                   sizeof(lines[0]), 2);
  TEST_ASSERT_EQUAL(2, n);
  TEST_ASSERT_EQUAL_STRING("Max", lines[0]);  // "Max Havelaar" is 12 > 10
  TEST_ASSERT_EQUAL_STRING("Havelaa...", lines[1]);
  n = wrapText("een twee drie", 60, measure6, nullptr, ell, lines[0], sizeof(lines[0]), 3);
  TEST_ASSERT_EQUAL(2, n);
  TEST_ASSERT_EQUAL_STRING("een twee", lines[0]);
  TEST_ASSERT_EQUAL_STRING("drie", lines[1]);
  n = wrapText("kort", 60, measure6, nullptr, ell, lines[0], sizeof(lines[0]), 2);
  TEST_ASSERT_EQUAL(1, n);
  TEST_ASSERT_EQUAL_STRING("kort", lines[0]);
  n = wrapText("abcdefghijklmnop qr", 60, measure6, nullptr, ell, lines[0], sizeof(lines[0]), 3);
  TEST_ASSERT_EQUAL(2, n);
  TEST_ASSERT_EQUAL_STRING("abcdefg...", lines[0]);  // over-long word cut
  TEST_ASSERT_EQUAL_STRING("qr", lines[1]);
  TEST_ASSERT_EQUAL(0, wrapText("   ", 60, measure6, nullptr, ell, lines[0], sizeof(lines[0]), 2));
}

// --- web_util ---------------------------------------------------------------

static void test_json_escape() {
  char out[64];
  TEST_ASSERT_TRUE(jsonEscape("Een \"ABC\" \\ tab\there", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("Een \\\"ABC\\\" \\\\ tab\\u0009here", out);
  TEST_ASSERT_TRUE(jsonEscape("\xC3\xA9\xC3\xA9n", out, sizeof(out)));  // UTF-8 passes through
  TEST_ASSERT_EQUAL_STRING("\xC3\xA9\xC3\xA9n", out);
  TEST_ASSERT_FALSE(jsonEscape("abcdef", out, 4));
  TEST_ASSERT_EQUAL_STRING("abc", out);
}

static void test_safe_book_name() {
  TEST_ASSERT_TRUE(isSafeBookName("max_havelaar.t5b"));
  TEST_ASSERT_TRUE(isSafeBookName("Mijn boek (2).T5B"));
  TEST_ASSERT_TRUE(isSafeBookName("Caf\xC3\xA9.t5b"));
  TEST_ASSERT_FALSE(isSafeBookName("../secrets.t5b"));
  TEST_ASSERT_FALSE(isSafeBookName("books/x.t5b"));
  TEST_ASSERT_FALSE(isSafeBookName(".hidden.t5b"));
  TEST_ASSERT_FALSE(isSafeBookName("book.epub"));
  TEST_ASSERT_FALSE(isSafeBookName(".t5b"));
  TEST_ASSERT_FALSE(isSafeBookName("a\nb.t5b"));
  char longName[80];
  memset(longName, 'a', 61);
  memcpy(longName + 61, ".t5b", 5);  // 65 bytes
  TEST_ASSERT_FALSE(isSafeBookName(longName));
}

static void test_safe_file_name_extensions() {
  const char *const fonts[] = {".ttf", ".otf", ".woff", ".woff2"};
  TEST_ASSERT_TRUE(isSafeFileName("Literata.woff2", fonts, 4));
  TEST_ASSERT_TRUE(isSafeFileName("My Font.TTF", fonts, 4));
  TEST_ASSERT_FALSE(isSafeFileName("font.woff3", fonts, 4));
  TEST_ASSERT_FALSE(isSafeFileName(".woff2", fonts, 4));
  const char *const sources[] = {".epub", ".txt"};
  TEST_ASSERT_TRUE(isSafeFileName("boek.epub", sources, 2));
  TEST_ASSERT_FALSE(isSafeFileName("boek.t5b", sources, 2));
  TEST_ASSERT_FALSE(isSafeFileName("../boek.epub", sources, 2));
}

static void test_wifi_qr_payload() {
  char out[96];
  TEST_ASSERT_TRUE(wifiQrPayload("T5-Reader-1A2B", "k7m4q9xp", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:T5-Reader-1A2B;P:k7m4q9xp;;", out);
  TEST_ASSERT_TRUE(wifiQrPayload("Thuis;5G", "a:b\\c", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:Thuis\\;5G;P:a\\:b\\\\c;;", out);
  TEST_ASSERT_FALSE(wifiQrPayload("ssid", "password", out, 20));
}

static void test_credentials() {
  char ssid[16], pass[9], code[5];
  makeSsid(0x12341A2B, ssid);
  TEST_ASSERT_EQUAL_STRING("T5-Reader-1A2B", ssid);
  makePassword(123, 456, pass);
  TEST_ASSERT_EQUAL(8, strlen(pass));
  for (int i = 0; i < 8; i++) TEST_ASSERT_NULL(strchr("ilo01", pass[i]));
  char again[9];
  makePassword(123, 456, again);
  TEST_ASSERT_EQUAL_STRING(pass, again);
  makePassword(124, 456, again);
  TEST_ASSERT_TRUE(strcmp(pass, again) != 0);
  makeCode(7, code);
  TEST_ASSERT_EQUAL_STRING("0007", code);
  makeCode(123456, code);
  TEST_ASSERT_EQUAL_STRING("3456", code);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_parse_golden_header_from_python_writer);
  RUN_TEST(test_header_errors);
  RUN_TEST(test_title_without_terminator_is_terminated);
  RUN_TEST(test_validate);
  RUN_TEST(test_chapter_for_page);
  RUN_TEST(test_book_id_and_key);
  RUN_TEST(test_short_press_with_bounce);
  RUN_TEST(test_long_press_fires_while_held_and_not_on_release);
  RUN_TEST(test_repeat);
  RUN_TEST(test_start_pressed_after_wake);
  RUN_TEST(test_tap_waits_for_the_double_tap_window);
  RUN_TEST(test_double_tap);
  RUN_TEST(test_hold_and_long_hold);
  RUN_TEST(test_tap_then_hold_is_not_a_double_tap);
  RUN_TEST(test_bounce_is_one_tap);
  RUN_TEST(test_click_decoder_after_wake);
  RUN_TEST(test_rotate_1bpp);
  RUN_TEST(test_refresh_policy);
  RUN_TEST(test_battery_percent);
  RUN_TEST(test_utf8_decode);
  RUN_TEST(test_ascii_fold);
  RUN_TEST(test_fit_text);
  RUN_TEST(test_format_progress);
  RUN_TEST(test_list_model_scrolls_and_wraps);
  RUN_TEST(test_settings_cycle_value_sanitize);
  RUN_TEST(test_wrap_text);
  RUN_TEST(test_json_escape);
  RUN_TEST(test_safe_book_name);
  RUN_TEST(test_safe_file_name_extensions);
  RUN_TEST(test_wifi_qr_payload);
  RUN_TEST(test_credentials);
  return UNITY_END();
}
