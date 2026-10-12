#include "ui.h"

#include <string.h>

#include "board.h"
#include "display.h"
#include "fonts/ui_font_12.h"
#include "fonts/ui_font_16.h"
#include "text_util.h"

namespace ui {
namespace {

constexpr int kTitleH = 16;
constexpr int kRowH = 15;
constexpr int kRowsTop = 18;
constexpr int kRowBaseline = 11;
constexpr int kFooterTop = 110;
constexpr int kSmallW = 6;  // built-in 6x8 font
const char *const kEllipsis = "\xE2\x80\xA6";

enum class Align { Left, Center, Right };

BBEPAPER &epd() { return display::epd(); }

int measure(const char *s, void *) {
  if (strlen(s) > 79) return 10000;  // bb_epaper measures in an 80-byte buffer
  BB_RECT r;
  epd().getStringBox(s, &r);
  return r.w;
}

// UTF-8 text in a converted font, cut to fit maxWidth; y is the baseline.
void text(const uint8_t *font, const char *s, int x, int baseline, int maxWidth, int color,
          Align align = Align::Left) {
  epd().setFont(font);
  char buf[96];
  fitText(s, maxWidth, measure, nullptr, kEllipsis, buf, sizeof(buf));
  const int w = measure(buf, nullptr);
  if (align == Align::Center) x += (maxWidth - w) / 2;
  if (align == Align::Right) x += maxWidth - w;
  epd().setTextColor(color, BBEP_TRANSPARENT);
  epd().drawString(buf, x, baseline);
}

// Built-in 6x8 font (ASCII only): UTF-8 is transliterated; y is the top row.
void small(const char *utf8, int x, int y, int maxChars, int color = BBEP_BLACK,
           int bg = BBEP_WHITE) {
  char ascii[64];
  asciiFold(utf8, ascii, sizeof(ascii));
  if (maxChars < 0) maxChars = 0;
  if (int(strlen(ascii)) > maxChars && maxChars < int(sizeof(ascii))) {
    ascii[maxChars] = '\0';
    if (maxChars >= 4) memcpy(ascii + maxChars - 3, "...", 3);
  }
  display::drawText6x8(ascii, x, y, color, bg);
}

void battery(int x, int y, int pct, int color) {  // 15 x 7
  epd().drawRect(x, y, 13, 7, color);
  epd().fillRect(x + 13, y + 2, 2, 3, color);
  if (pct > 0) epd().fillRect(x + 2, y + 2, (pct * 9 + 50) / 100, 3, color);
}

void titleBar(const char *title, int batteryPct) {
  epd().fillRect(0, 0, kPageW, kTitleH, BBEP_BLACK);
  text(ui_font_12, title, 4, 12, kPageW - 30, BBEP_WHITE);
  if (batteryPct >= 0) battery(kPageW - 19, 5, batteryPct, BBEP_WHITE);
}

void footer(const char *hint) {
  if (!hint || !*hint) return;
  epd().drawLine(0, kFooterTop, kPageW - 1, kFooterTop, BBEP_BLACK);
  small(hint, 2, kFooterTop + 3, (kPageW - 4) / kSmallW);
}

void arrow(int cx, int y, bool up) {  // 7 px wide
  for (int i = 0; i < 4; i++) {
    const int row = up ? y + i : y + 3 - i;
    epd().drawLine(cx - i, row, cx + i, row, BBEP_BLACK);
  }
}

}  // namespace

void statusBar(const char *chapter, uint32_t page, uint32_t pageCount, int batteryPct) {
  const int top = kPageH - kStatusBarH;
  epd().fillRect(0, top, kPageW, kStatusBarH, BBEP_WHITE);
  epd().drawLine(0, top, kPageW - 1, top, BBEP_BLACK);
  const int batteryX = kPageW - 17;
  if (batteryPct >= 0) battery(batteryX, top + 2, batteryPct, BBEP_BLACK);
  char progress[24];
  formatProgress(page, pageCount, progress, sizeof(progress));
  const int progressX = batteryX - 4 - int(strlen(progress)) * kSmallW;
  small(progress, progressX, top + 2, 24);
  small(chapter ? chapter : "", 2, top + 2, (progressX - 8) / kSmallW);
}

void menu(const char *title, const ListModel &model, ItemText itemText, void *ctx,
          const char *hint, int batteryPct) {
  display::clear();
  titleBar(title, batteryPct);
  const int textW = kPageW - 16;
  for (int r = 0; r < model.visibleRows() && model.first() + r < model.count(); r++) {
    const int i = model.first() + r;
    const int top = kRowsTop + r * kRowH;
    const bool selected = i == model.selected();
    if (selected) epd().fillRect(0, top, kPageW - 9, kRowH, BBEP_BLACK);
    char item[96];
    itemText(i, item, sizeof(item), ctx);
    text(ui_font_12, item, 4, top + kRowBaseline, textW, selected ? BBEP_WHITE : BBEP_BLACK);
  }
  if (model.canScrollUp()) arrow(kPageW - 4, kRowsTop + 1, true);
  if (model.canScrollDown()) arrow(kPageW - 4, kRowsTop + kMenuRows * kRowH - 6, false);
  if (model.count() == 0) text(ui_font_12, "(empty)", 4, kRowsTop + kRowBaseline, textW, BBEP_BLACK);
  footer(hint);
}

void message(const char *title, const char *line1, const char *line2, const char *hint) {
  display::clear();
  titleBar(title, -1);
  if (line1) text(ui_font_12, line1, 4, 52, kPageW - 8, BBEP_BLACK, Align::Center);
  if (line2) text(ui_font_12, line2, 4, 72, kPageW - 8, BBEP_BLACK, Align::Center);
  footer(hint);
}

void textScreen(const char *title, const char *const *lines, int count, const char *hint) {
  display::clear();
  titleBar(title, -1);
  for (int i = 0; i < count && i < kMenuRows; i++) {
    text(ui_font_12, lines[i], 4, kRowsTop + i * kRowH + kRowBaseline, kPageW - 8, BBEP_BLACK);
  }
  footer(hint);
}

void valueEditor(const char *title, const char *value, int percent, const char *hint) {
  display::clear();
  titleBar(title, -1);
  text(ui_font_16, value, 0, 58, kPageW, BBEP_BLACK, Align::Center);
  epd().drawRect(20, 72, kPageW - 40, 12, BBEP_BLACK);
  const int fill = (kPageW - 44) * (percent < 0 ? 0 : percent > 100 ? 100 : percent) / 100;
  if (fill > 0) epd().fillRect(22, 74, fill, 8, BBEP_BLACK);
  footer(hint);
}

void sleepScreen(const char *title, const char *author, int batteryPct) {
  display::clear();
  epd().drawRect(0, 0, kPageW, kPageH, BBEP_BLACK);
  epd().drawRect(3, 3, kPageW - 6, kPageH - 6, BBEP_BLACK);
  text(ui_font_16, title && *title ? title : "T5 Reader", 10, 52, kPageW - 20, BBEP_BLACK,
       Align::Center);
  if (author && *author) {
    text(ui_font_12, author, 10, 74, kPageW - 20, BBEP_BLACK, Align::Center);
  }
  const char *hint = "sleeping - press a button";
  small(hint, (kPageW - int(strlen(hint)) * kSmallW) / 2, 98, 40);
  if (batteryPct >= 0) battery(kPageW - 24, 9, batteryPct, BBEP_BLACK);
}

}  // namespace ui
