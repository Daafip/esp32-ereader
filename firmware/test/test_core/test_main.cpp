// Host unit tests for lib/core: `pixi run fw-test`.
#include <string.h>
#include <unity.h>

#include "button.h"
#include "golden_header.h"
#include "menu.h"
#include "policy.h"
#include "settings.h"
#include "t5b_format.h"
#include "text_util.h"

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
  TEST_ASSERT_EQUAL_STRING("1/3596  0%", out);
  formatProgress(3595, 3596, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("3596/3596  100%", out);
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

static void test_settings_cycle_describe_sanitize() {
  Settings s;
  char text[48];
  describeSetting(s, SettingId::FullEvery, text, sizeof(text));
  TEST_ASSERT_EQUAL_STRING("Full refresh: every 10", text);
  cycleSetting(s, SettingId::FullEvery);
  TEST_ASSERT_EQUAL(15, s.fullEvery);
  for (int i = 0; i < 3; i++) cycleSetting(s, SettingId::FullEvery);
  TEST_ASSERT_EQUAL(0, s.fullEvery);
  describeSetting(s, SettingId::FullEvery, text, sizeof(text));
  TEST_ASSERT_EQUAL_STRING("Full refresh: chapters only", text);
  cycleSetting(s, SettingId::FullEvery);
  TEST_ASSERT_EQUAL(5, s.fullEvery);

  cycleSetting(s, SettingId::WakeRefresh);
  TEST_ASSERT_EQUAL(int(WakeRefresh::Retain), int(s.wakeRefresh));
  cycleSetting(s, SettingId::DeepSleep);
  describeSetting(s, SettingId::DeepSleep, text, sizeof(text));
  TEST_ASSERT_EQUAL_STRING("Sleep after page: off", text);

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
  RUN_TEST(test_refresh_policy);
  RUN_TEST(test_battery_percent);
  RUN_TEST(test_utf8_decode);
  RUN_TEST(test_ascii_fold);
  RUN_TEST(test_fit_text);
  RUN_TEST(test_format_progress);
  RUN_TEST(test_list_model_scrolls_and_wraps);
  RUN_TEST(test_settings_cycle_describe_sanitize);
  return UNITY_END();
}
