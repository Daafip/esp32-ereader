// Host unit tests for lib/core: `pixi run fw-test`.
#include <string.h>
#include <unity.h>

#include "bitmap.h"
#include "board_tag.h"
#include "bookmarks.h"
#include "button.h"
#include "golden_header.h"
#include "i18n.h"
#include "menu.h"
#include "policy.h"
#include "reading_stats.h"
#include "rtc_time.h"
#include "settings.h"
#include "t5b_format.h"
#include "text_util.h"
#include "touch_gesture.h"
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

// --- touch gestures --------------------------------------------------------------

// Finger down from `from` to `to` (exclusive) moving linearly (x0,y0) -> (x1,y1),
// 10 ms samples, then released at `to`. Returns every event seen.
static int stroke(TouchTracker &t, uint32_t from, uint32_t to, int x0, int y0, int x1, int y1,
                  TouchEvent *events, int max) {
  int n = 0;
  for (uint32_t now = from; now < to; now += 10) {
    const int x = x0 + int((x1 - x0) * int(now - from) / int(to - from));
    const int y = y0 + int((y1 - y0) * int(now - from) / int(to - from));
    const TouchEvent e = t.update(true, x, y, now);
    if (e.kind != TouchKind::None && n < max) events[n++] = e;
  }
  const TouchEvent e = t.update(false, 0, 0, to);
  if (e.kind != TouchKind::None && n < max) events[n++] = e;
  return n;
}

static void test_touch_tap_and_jitter() {
  TouchTracker t;  // long 600 ms, slop 24, swipe 80
  TouchEvent ev[4];
  TEST_ASSERT_EQUAL(1, stroke(t, 0, 120, 300, 500, 310, 495, ev, 4));
  TEST_ASSERT_EQUAL(int(TouchKind::Tap), int(ev[0].kind));
  TEST_ASSERT_EQUAL(300, ev[0].x);  // where the finger went down
  TEST_ASSERT_EQUAL(500, ev[0].y);
  TEST_ASSERT_FALSE(t.isDown());
}

static void test_touch_long_press_fires_once_while_down() {
  TouchTracker t;
  TouchEvent ev[4];
  TEST_ASSERT_EQUAL(1, stroke(t, 0, 1500, 100, 100, 104, 100, ev, 4));
  TEST_ASSERT_EQUAL(int(TouchKind::LongPress), int(ev[0].kind));  // nothing on release
}

static void test_touch_swipes() {
  TouchTracker t;
  TouchEvent ev[4];
  TEST_ASSERT_EQUAL(1, stroke(t, 0, 200, 450, 500, 200, 520, ev, 4));
  TEST_ASSERT_EQUAL(int(TouchKind::SwipeLeft), int(ev[0].kind));
  TEST_ASSERT_EQUAL(1, stroke(t, 1000, 1200, 100, 500, 400, 470, ev, 4));
  TEST_ASSERT_EQUAL(int(TouchKind::SwipeRight), int(ev[0].kind));
  TEST_ASSERT_EQUAL(1, stroke(t, 2000, 2200, 270, 800, 260, 300, ev, 4));
  TEST_ASSERT_EQUAL(int(TouchKind::SwipeUp), int(ev[0].kind));
  // a slow drag that turns into a swipe never fires the long press
  TEST_ASSERT_EQUAL(1, stroke(t, 3000, 4000, 100, 500, 500, 500, ev, 4));
  TEST_ASSERT_EQUAL(int(TouchKind::SwipeRight), int(ev[0].kind));
  // diagonal or too short: nothing
  TEST_ASSERT_EQUAL(0, stroke(t, 5000, 5200, 100, 100, 300, 300, ev, 4));
  TEST_ASSERT_EQUAL(0, stroke(t, 6000, 6200, 100, 100, 150, 100, ev, 4));
}

static void test_reading_tap_zones() {
  TEST_ASSERT_EQUAL(int(ReadingTap::Prev), int(readingTap(10, 500, 540, 960)));
  TEST_ASSERT_EQUAL(int(ReadingTap::Next), int(readingTap(200, 500, 540, 960)));
  TEST_ASSERT_EQUAL(int(ReadingTap::Next), int(readingTap(530, 950, 540, 960)));
  TEST_ASSERT_EQUAL(int(ReadingTap::Menu), int(readingTap(10, 20, 540, 960)));
  TEST_ASSERT_EQUAL(int(ReadingTap::Menu), int(readingTap(400, 119, 540, 960)));
}

// --- board tag ----------------------------------------------------------------------

static void test_board_tag_found_across_chunks() {
  static const char kPrefix[] = "T5READER-BOARD:";
  const char image[] = "\xE9\x05junk T5READ T5READER-BOARD:m5paper\0more T5READER-BOARD:t5-213";
  BoardTagScanner scan(kPrefix, sizeof(kPrefix) - 1);
  const size_t chunks[] = {1, 3, 7, sizeof(image)};
  for (size_t chunk : chunks) {
    scan.reset();
    for (size_t i = 0; i < sizeof(image); i += chunk) {
      const size_t n = i + chunk > sizeof(image) ? sizeof(image) - i : chunk;
      scan.feed(reinterpret_cast<const uint8_t *>(image) + i, n);
    }
    TEST_ASSERT_TRUE(scan.found());
    TEST_ASSERT_EQUAL_STRING("m5paper", scan.name());  // the first one counts
  }
}

static void test_board_tag_absent() {
  static const char kPrefix[] = "T5READER-BOARD:";
  const char image[] = "no tag here, T5READER-BOARD: (empty) T5READER-BOARX:t5";
  BoardTagScanner scan(kPrefix, sizeof(kPrefix) - 1);
  scan.feed(reinterpret_cast<const uint8_t *>(image), sizeof(image));
  TEST_ASSERT_FALSE(scan.found());
  TEST_ASSERT_EQUAL_STRING("", scan.name());
}

// --- cover block, folders, bookmarks, statistics -------------------------------------

static void test_cover_block() {
  t5b::Header h;
  TEST_ASSERT_EQUAL(int(t5b::Error::None), int(t5b::parseHeader(kGoldenHeader, 146, h)));
  const uint32_t end = h.fileSize();
  t5b::Cover c;
  TEST_ASSERT_EQUAL(int(t5b::Error::None), int(t5b::parseCover(h, end, nullptr, 0, c)));
  TEST_ASSERT_FALSE(c.present);
  // 20 x 3 cover: stride 3, 9 bytes of bitmap
  const uint8_t head[12] = {'T', '5', 'C', 'V', 20, 0, 3, 0, 3, 0, 0, 0};
  TEST_ASSERT_EQUAL(int(t5b::Error::None), int(t5b::validate(h, 250, 122, end + 12 + 9)));
  TEST_ASSERT_EQUAL(int(t5b::Error::None), int(t5b::parseCover(h, end + 12 + 9, head, 12, c)));
  TEST_ASSERT_TRUE(c.present);
  TEST_ASSERT_EQUAL(20, c.w);
  TEST_ASSERT_EQUAL(3, c.h);
  TEST_ASSERT_EQUAL(end + 12, c.offset);
  // wrong length, wrong magic, wrong stride, file too short
  TEST_ASSERT_EQUAL(int(t5b::Error::SizeMismatch), int(t5b::parseCover(h, end + 12 + 8, head, 12, c)));
  uint8_t bad[12];
  memcpy(bad, head, 12);
  bad[0] = 'X';
  TEST_ASSERT_EQUAL(int(t5b::Error::SizeMismatch), int(t5b::parseCover(h, end + 21, bad, 12, c)));
  memcpy(bad, head, 12);
  bad[8] = 4;
  TEST_ASSERT_EQUAL(int(t5b::Error::SizeMismatch), int(t5b::parseCover(h, end + 24, bad, 12, c)));
  TEST_ASSERT_EQUAL(int(t5b::Error::SizeMismatch), int(t5b::validate(h, 250, 122, end - 1)));
}

static void test_book_paths_and_folders() {
  TEST_ASSERT_TRUE(isSafeFolderName("Classics"));
  TEST_ASSERT_TRUE(isSafeFolderName("Nederlands 19e eeuw"));
  TEST_ASSERT_FALSE(isSafeFolderName(".src"));
  TEST_ASSERT_FALSE(isSafeFolderName("a/b"));
  TEST_ASSERT_FALSE(isSafeFolderName(""));
  TEST_ASSERT_FALSE(isSafeFolderName("trailing "));
  TEST_ASSERT_TRUE(isSafeBookPath("boek.t5b"));
  TEST_ASSERT_TRUE(isSafeBookPath("Classics/boek.t5b"));
  TEST_ASSERT_FALSE(isSafeBookPath("a/b/boek.t5b"));
  TEST_ASSERT_FALSE(isSafeBookPath(".marks/boek.t5b"));
  TEST_ASSERT_FALSE(isSafeBookPath("/boek.t5b"));
  TEST_ASSERT_FALSE(isSafeBookPath("Classics/boek.epub"));
  TEST_ASSERT_FALSE(isSafeBookPath("../boek.t5b"));
}

static void test_page_list() {
  PageList l;
  TEST_ASSERT_TRUE(l.add(40));
  TEST_ASSERT_TRUE(l.add(3));
  TEST_ASSERT_TRUE(l.add(17));
  TEST_ASSERT_FALSE(l.add(17));
  TEST_ASSERT_EQUAL(3, l.count());
  TEST_ASSERT_EQUAL(3, l.at(0));
  TEST_ASSERT_EQUAL(40, l.at(2));
  char text[64];
  TEST_ASSERT_EQUAL(8, l.format(text, sizeof(text)));
  TEST_ASSERT_EQUAL_STRING("3\n17\n40\n", text);
  TEST_ASSERT_TRUE(l.remove(17));
  TEST_ASSERT_FALSE(l.contains(17));

  PageList r;
  const char in[] = "12\r\n5\nxx\n5\n999\n\n7";
  r.parse(in, sizeof(in) - 1, 100);
  TEST_ASSERT_EQUAL(3, r.count());  // 999 out of range, junk and duplicate skipped
  TEST_ASSERT_EQUAL(5, r.at(0));
  TEST_ASSERT_EQUAL(12, r.at(2));

  r.remap(100, 199);  // 5 -> 10, 7 -> 14, 12 -> 24
  TEST_ASSERT_EQUAL(10, r.at(0));
  TEST_ASSERT_EQUAL(24, r.at(2));
  TEST_ASSERT_EQUAL(0, remapPage(5, 1, 50));
  TEST_ASSERT_EQUAL(49, remapPage(99, 100, 50));
  TEST_ASSERT_EQUAL(0, remapPage(3, 10, 0));
}

static void test_reading_stats() {
  const uint32_t day0 = 1760000000u - 1760000000u % 86400;  // a UTC midnight
  ReadingStats st;
  recordPage(st, day0 + 3600, 0);
  recordPage(st, day0 + 3660, 0);   // 60 s on the page
  recordPage(st, day0 + 9000, 0);   // a pause: no time
  TEST_ASSERT_EQUAL(3, st.pages);
  TEST_ASSERT_EQUAL(60, st.seconds);
  TEST_ASSERT_EQUAL(1, st.daysRead);
  TEST_ASSERT_EQUAL(1, currentStreak(st, day0 + 9000, 0));
  TEST_ASSERT_EQUAL(3, todayPages(st, day0 + 9000, 0));

  recordPage(st, day0 + 86400 + 100, 0);  // next day
  TEST_ASSERT_EQUAL(2, st.streak);
  TEST_ASSERT_EQUAL(1, todayPages(st, day0 + 86400 + 200, 0));
  TEST_ASSERT_EQUAL(2, currentStreak(st, day0 + 2 * 86400 + 5, 0));  // not broken yet today
  TEST_ASSERT_EQUAL(0, currentStreak(st, day0 + 3 * 86400 + 5, 0));  // a day missed
  recordPage(st, day0 + 3 * 86400 + 10, 0);
  TEST_ASSERT_EQUAL(1, st.streak);
  TEST_ASSERT_EQUAL(2, st.bestStreak);
  TEST_ASSERT_EQUAL(3, st.daysRead);

  // time zone: 23:30 UTC is already tomorrow at UTC+2
  TEST_ASSERT_EQUAL(localDay(day0 + 86400, 0), localDay(day0 + 84600, 7200));
  // no clock: pages and time count, days don't
  ReadingStats nc;
  recordPage(nc, 1000, 0);
  recordPage(nc, 1030, 0);
  TEST_ASSERT_EQUAL(2, nc.pages);
  TEST_ASSERT_EQUAL(30, nc.seconds);
  TEST_ASSERT_EQUAL(0, nc.daysRead);
  TEST_ASSERT_EQUAL(-1, localDay(1030, 0));
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

static void test_format_bytes() {
  char out[16];
  formatBytes(0, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("0 KB", out);
  formatBytes(1536, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("2 KB", out);
  formatBytes(740ull << 20, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("740 MB", out);
  formatBytes((62ull << 30) / 10, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("6.2 GB", out);
  formatBytes(1ull << 30, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("1.0 GB", out);
  formatBytes(119ull << 30, out, sizeof(out));  // a "128 GB" card
  TEST_ASSERT_EQUAL_STRING("119.0 GB", out);
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

static void test_settings_options_keys_and_language() {
  setLanguage(Lang::En);
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
  cycleSetting(s, SettingId::DeepSleep);
  settingValue(s, SettingId::DeepSleep, text, sizeof(text));
  TEST_ASSERT_EQUAL_STRING("off", text);
  settingValue(s, SettingId::LongPress, text, sizeof(text));
  TEST_ASSERT_EQUAL_STRING("600 ms", text);
  TEST_ASSERT_EQUAL_STRING("Long press", settingLabel(SettingId::HoldAction, S::SetLongPress));

  TEST_ASSERT_FALSE(s.flip);  // the screen the right way up by default
  cycleSetting(s, SettingId::Flip);
  TEST_ASSERT_TRUE(s.flip);
  settingValue(s, SettingId::Flip, text, sizeof(text));
  TEST_ASSERT_EQUAL_STRING("on", text);
  TEST_ASSERT_EQUAL_STRING("Flip screen", settingLabel(SettingId::Flip));
  SettingId flipId;
  TEST_ASSERT_TRUE(settingFromKey("flip", flipId));
  TEST_ASSERT_EQUAL(int(SettingId::Flip), int(flipId));

  // every option round-trips through its index, every key back to its id
  for (int id = 0; id < kSettingCount; id++) {
    const SettingId sid = SettingId(id);
    TEST_ASSERT_TRUE(settingOptionCount(sid) >= 2);
    for (int i = 0; i < settingOptionCount(sid); i++) {
      Settings t;
      setSettingIndex(t, sid, i);
      TEST_ASSERT_EQUAL(i, settingIndex(t, sid));
      settingOptionText(sid, i, text, sizeof(text));
      TEST_ASSERT_TRUE(text[0] != '\0');
    }
    SettingId back;
    TEST_ASSERT_TRUE(settingFromKey(settingKey(sid), back));
    TEST_ASSERT_EQUAL(id, int(back));
    TEST_ASSERT_TRUE(strlen(settingKey(sid)) <= 13);  // NVS keys: "s_" + key <= 15
  }
  SettingId none;
  TEST_ASSERT_FALSE(settingFromKey("nope", none));

  // values outside the lists (old NVS data) read as the first option; bad indexes are ignored
  s.fullEvery = 7;
  TEST_ASSERT_EQUAL(0, settingIndex(s, SettingId::FullEvery));
  setSettingIndex(s, SettingId::FullEvery, 99);
  TEST_ASSERT_EQUAL(7, s.fullEvery);

  // Dutch
  setLanguage(Lang::Nl);
  TEST_ASSERT_EQUAL_STRING("Volledig verversen", settingLabel(SettingId::FullEvery));
  s = Settings();
  settingValue(s, SettingId::FullEvery, text, sizeof(text));
  TEST_ASSERT_EQUAL_STRING("elke 10", text);
  settingValue(s, SettingId::Language, text, sizeof(text));
  TEST_ASSERT_EQUAL_STRING("English", text);  // each language in its own name
  setLanguage(Lang::En);
}

static void test_translations_complete() {
  for (int i = 0; i < int(S::Count); i++) {
    TEST_ASSERT_NOT_NULL(tr(S(i), Lang::En));
    TEST_ASSERT_NOT_NULL(tr(S(i), Lang::Nl));
    TEST_ASSERT_TRUE(tr(S(i), Lang::En)[0] != '\0');
    TEST_ASSERT_TRUE(tr(S(i), Lang::Nl)[0] != '\0');
  }
  TEST_ASSERT_EQUAL_STRING("Bladwijzers", tr(S::Bookmarks, Lang::Nl));
  TEST_ASSERT_EQUAL_STRING("?", tr(S::Count, Lang::En));
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

void test_rtc_time() {
  TEST_ASSERT_EQUAL_INT32(0, daysFromCivil(1970, 1, 1));
  TEST_ASSERT_EQUAL_INT32(10957, daysFromCivil(2000, 1, 1));
  TEST_ASSERT_EQUAL_INT32(11017, daysFromCivil(2000, 3, 1));  // 2000 is a leap year
  int32_t y;
  unsigned m, d;
  civilFromDays(daysFromCivil(2026, 10, 13), y, m, d);
  TEST_ASSERT_EQUAL_INT32(2026, y);
  TEST_ASSERT_EQUAL_UINT(10, m);
  TEST_ASSERT_EQUAL_UINT(13, d);

  // 2026-10-13 21:07:45 UTC, a Tuesday
  const uint32_t t = uint32_t(daysFromCivil(2026, 10, 13)) * 86400 + 21 * 3600 + 7 * 60 + 45;
  uint8_t r[kRtcRegCount];
  encodeRtc(t, r);
  const uint8_t want[kRtcRegCount] = {0x45, 0x07, 0x21, 0x13, 2, 0x10, 0x26};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, r, kRtcRegCount);
  uint32_t back = 0;
  TEST_ASSERT_TRUE(decodeRtc(r, back));
  TEST_ASSERT_EQUAL_UINT32(t, back);

  // leap day, last second of a year
  const uint32_t edges[] = {uint32_t(daysFromCivil(2028, 2, 29)) * 86400 + 43200,
                            uint32_t(daysFromCivil(2030, 1, 1)) * 86400 - 1};
  for (uint32_t e : edges) {
    encodeRtc(e, r);
    TEST_ASSERT_TRUE(decodeRtc(r, back));
    TEST_ASSERT_EQUAL_UINT32(e, back);
  }

  r[0] |= 0x80;  // VL: the clock stopped
  TEST_ASSERT_FALSE(decodeRtc(r, back));
  const uint8_t junk[kRtcRegCount] = {0x00, 0x00, 0x00, 0x00, 0, 0x00, 0x00};  // day 0
  TEST_ASSERT_FALSE(decodeRtc(junk, back));
  const uint8_t notBcd[kRtcRegCount] = {0x5A, 0x00, 0x00, 0x01, 0, 0x01, 0x26};
  TEST_ASSERT_FALSE(decodeRtc(notBcd, back));
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
  RUN_TEST(test_cover_block);
  RUN_TEST(test_book_paths_and_folders);
  RUN_TEST(test_page_list);
  RUN_TEST(test_reading_stats);
  RUN_TEST(test_rtc_time);
  RUN_TEST(test_board_tag_found_across_chunks);
  RUN_TEST(test_board_tag_absent);
  RUN_TEST(test_touch_tap_and_jitter);
  RUN_TEST(test_touch_long_press_fires_once_while_down);
  RUN_TEST(test_touch_swipes);
  RUN_TEST(test_reading_tap_zones);
  RUN_TEST(test_refresh_policy);
  RUN_TEST(test_battery_percent);
  RUN_TEST(test_utf8_decode);
  RUN_TEST(test_ascii_fold);
  RUN_TEST(test_fit_text);
  RUN_TEST(test_format_progress);
  RUN_TEST(test_format_bytes);
  RUN_TEST(test_list_model_scrolls_and_wraps);
  RUN_TEST(test_settings_options_keys_and_language);
  RUN_TEST(test_translations_complete);
  RUN_TEST(test_wrap_text);
  RUN_TEST(test_json_escape);
  RUN_TEST(test_safe_book_name);
  RUN_TEST(test_safe_file_name_extensions);
  RUN_TEST(test_wifi_qr_payload);
  RUN_TEST(test_credentials);
  return UNITY_END();
}
