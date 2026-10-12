#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "display.h"
#include "fonts/ui_body.h"
#include "fonts/ui_display.h"
#include "fonts/ui_small.h"
#include "fonts/ui_title.h"
#include "text_util.h"

namespace ui {
namespace {

constexpr int kW = kPageW;
constexpr int kH = kPageH;
constexpr int kMargin = 8;
constexpr int kHeaderBaseline = 14;
constexpr int kHeaderRule = 19;
constexpr int kRowsTop = 22;
constexpr int kRowH = 17;
constexpr int kRowBaseline = 12;
constexpr int kFooterTop = kH - 13;
constexpr int kChipH = 12;
const char *const kEllipsis = "\xE2\x80\xA6";
constexpr int kBlack = BBEP_BLACK;
constexpr int kWhite = BBEP_WHITE;

enum class Align { Left, Center, Right };

BBEPAPER &epd() { return display::epd(); }

int measure(const char *s, void *) {
  if (strlen(s) > 79) return 10000;  // bb_epaper measures in an 80-byte buffer
  BB_RECT r;
  epd().getStringBox(s, &r);
  return r.w;
}

int width(const uint8_t *font, const char *s) {
  epd().setFont(font);
  return measure(s, nullptr);
}

// UTF-8 text cut to fit maxWidth; y is the baseline. Returns the drawn width.
int text(const uint8_t *font, const char *s, int x, int baseline, int maxWidth, int color,
         Align align = Align::Left) {
  if (!s || !*s || maxWidth <= 0) return 0;
  epd().setFont(font);
  char buf[96];
  fitText(s, maxWidth, measure, nullptr, kEllipsis, buf, sizeof(buf));
  const int w = measure(buf, nullptr);
  if (align == Align::Center) x += (maxWidth - w) / 2;
  if (align == Align::Right) x += maxWidth - w;
  epd().setTextColor(color, BBEP_TRANSPARENT);
  epd().drawString(buf, x, baseline);
  return w;
}

// --- small vector icons ---------------------------------------------------------

void batteryIcon(int x, int y, int pct, int color) {  // 16 x 8
  epd().drawRoundRect(x, y, 14, 8, 2, color);
  epd().fillRect(x + 14, y + 2, 2, 4, color);
  const int fill = pct <= 0 ? 0 : (pct * 10 + 50) / 100;
  if (fill > 0) epd().fillRect(x + 2, y + 2, fill, 4, color);
}

// Battery percentage and icon, right-aligned at `right`. Returns the left edge.
int battery(int right, int baseline, int pct, int color) {
  if (pct < 0) return right;
  batteryIcon(right - 16, baseline - 8, pct, color);
  char label[12];
  snprintf(label, sizeof(label), "%d%%", pct);
  const int w = width(ui_small, label);
  text(ui_small, label, right - 16 - 4 - w, baseline, w, color);
  return right - 16 - 4 - w;
}

void chevron(int x, int cy, int color) {  // 5 x 7, two pixels thick
  for (int dx = 0; dx < 2; dx++) {
    epd().drawLine(x + dx, cy - 3, x + dx + 3, cy, color);
    epd().drawLine(x + dx + 3, cy, x + dx, cy + 3, color);
  }
}

void moon(int cx, int cy) {
  epd().fillCircle(cx, cy, 4, kBlack);
  epd().fillCircle(cx + 2, cy - 2, 3, kWhite);
}

void icon(Icon which, int cx, int top) {  // about 28 x 26
  BBEPAPER &d = epd();
  switch (which) {
    case Icon::Card: {  // SD card: rounded body, cut corner, contacts
      const int x = cx - 10, y = top;
      d.drawRoundRect(x, y, 20, 26, 3, kBlack);
      d.drawRoundRect(x + 1, y + 1, 18, 24, 2, kBlack);
      d.fillRect(x, y, 7, 7, kWhite);
      d.drawLine(x, y + 6, x + 6, y, kBlack);
      d.drawLine(x + 1, y + 6, x + 6, y + 1, kBlack);
      for (int i = 0; i < 4; i++) d.fillRect(x + 7 + i * 3, y + 3, 2, 5, kBlack);
      break;
    }
    case Icon::Book: {  // closed book with spine and two title lines
      const int x = cx - 10, y = top;
      d.drawRoundRect(x, y, 20, 26, 2, kBlack);
      d.fillRect(x, y, 4, 26, kBlack);
      d.drawLine(x + 8, y + 7, x + 16, y + 7, kBlack);
      d.drawLine(x + 8, y + 10, x + 14, y + 10, kBlack);
      break;
    }
    case Icon::Battery: {  // large, nearly empty
      const int x = cx - 16, y = top + 6;
      d.drawRoundRect(x, y, 29, 15, 3, kBlack);
      d.drawRoundRect(x + 1, y + 1, 27, 13, 2, kBlack);
      d.fillRect(x + 29, y + 4, 3, 7, kBlack);
      d.fillRect(x + 4, y + 4, 4, 7, kBlack);
      break;
    }
    case Icon::Warning: {  // triangle with an exclamation mark
      for (int t = 0; t < 2; t++) {
        d.drawLine(cx, top + t, cx - 13 + t, top + 24 - t, kBlack);
        d.drawLine(cx, top + t, cx + 13 - t, top + 24 - t, kBlack);
        d.drawLine(cx - 13 + t, top + 24 - t, cx + 13 - t, top + 24 - t, kBlack);
      }
      d.fillRect(cx - 1, top + 8, 3, 9, kBlack);
      d.fillRect(cx - 1, top + 19, 3, 2, kBlack);
      break;
    }
  }
}

// --- chrome -------------------------------------------------------------------------

void header(const char *title, int batteryPct) {
  const int left = battery(kW - kMargin, kHeaderBaseline - 1, batteryPct, kBlack);
  text(ui_title, title, kMargin, kHeaderBaseline, left - kMargin - 10, kBlack);
  epd().drawLine(kMargin, kHeaderRule, kW - kMargin - 1, kHeaderRule, kBlack);
}

// Footer arrow (7 x 4), vertically centred on the chips.
void arrow(int x, bool up) {
  const int cy = kFooterTop + kChipH / 2;
  for (int i = 0; i < 4; i++) {
    const int y = up ? cy - 2 + i : cy + 1 - i;
    epd().drawLine(x + 3 - i, y, x + 3 + i, y, kBlack);
  }
}

// A key chip: filled (the button) or outlined (the "hold" modifier), then a label
// (text or an arrow). Returns the width; draw = false only measures.
int chip(const char *key, const char *label, bool filled, int x, bool draw) {
  const bool isArrow = label == kArrowDown || label == kArrowUp;
  const int kw = width(ui_small, key);
  const int lw = isArrow ? 7 : (label ? width(ui_small, label) : 0);
  if (draw) {
    if (filled) {
      epd().fillRoundRect(x, kFooterTop, kw + 10, kChipH, 5, kBlack);
    } else {
      epd().drawRoundRect(x, kFooterTop, kw + 10, kChipH, 5, kBlack);
    }
    text(ui_small, key, x + 5, kFooterTop + 9, kw, filled ? kWhite : kBlack);
    if (isArrow) {
      arrow(x + kw + 14, label == kArrowUp);
    } else if (label) {
      text(ui_small, label, x + kw + 14, kFooterTop + 9, lw, kBlack);
    }
  }
  return kw + 10 + (lw ? 4 + lw : 0);
}

// "[KEY] action (hold) holdAction"; returns the width.
int hintGroup(const Hint &h, int x, bool draw) {
  if (!h.key) return 0;
  int w = chip(h.key, h.action, true, x, draw);
  if (h.holdAction) w += 7 + chip("hold", h.holdAction, false, x + w + 7, draw);
  return w;
}

void footer(Hint left, Hint right) {
  // Too wide for one row? Drop the long-press chips, right group first.
  const int room = kW - 2 * kMargin - 8;
  if (hintGroup(left, 0, false) + hintGroup(right, 0, false) > room) right.holdAction = nullptr;
  if (hintGroup(left, 0, false) + hintGroup(right, 0, false) > room) left.holdAction = nullptr;
  hintGroup(left, kMargin, true);
  const int w = hintGroup(right, 0, false);
  if (w) hintGroup(right, kW - kMargin - w, true);
}

void scrollbar(const ListModel &m) {
  if (m.count() <= m.visibleRows()) return;
  const int x = kW - 4;
  const int trackH = m.visibleRows() * kRowH;
  for (int y = kRowsTop; y < kRowsTop + trackH; y += 2) epd().drawPixel(x, y, kBlack);
  int thumbH = trackH * m.visibleRows() / m.count();
  if (thumbH < 8) thumbH = 8;
  const int range = m.count() - m.visibleRows();
  const int y = kRowsTop + (trackH - thumbH) * m.first() / range;
  epd().fillRoundRect(x - 1, y, 3, thumbH, 1, kBlack);
}

}  // namespace

void statusBar(const char *chapter, uint32_t page, uint32_t pageCount, int batteryPct) {
  const int top = kH - kStatusBarH;
  const int baseline = kH - 3;
  epd().fillRect(0, top, kW, kStatusBarH, kWhite);
  // reading progress: dotted track, solid fill
  const int x0 = 4, x1 = kW - 5;
  for (int x = x0; x <= x1; x += 2) epd().drawPixel(x, top + 1, kBlack);
  if (pageCount) {
    const int fill = int(uint64_t(x1 - x0 + 1) * (page + 1) / pageCount);
    if (fill > 0) epd().fillRect(x0, top, fill, 2, kBlack);
  }
  int right = x1 + 1;
  if (batteryPct >= 0) {
    batteryIcon(right - 16, baseline - 7, batteryPct, kBlack);
    right -= 16 + 7;
  }
  char progress[32];
  formatProgress(page, pageCount, progress, sizeof(progress));
  const int pw = width(ui_small, progress);
  text(ui_small, progress, right - pw, baseline, pw, kBlack);
  text(ui_small, chapter, x0, baseline, right - pw - 12 - x0, kBlack);
}

void menu(const char *title, const ListModel &model, ItemFn item, void *ctx, Hint left,
          Hint right, int batteryPct) {
  display::clear();
  header(title, batteryPct);
  for (int r = 0; r < model.visibleRows() && model.first() + r < model.count(); r++) {
    const int i = model.first() + r;
    const int top = kRowsTop + r * kRowH;
    const bool selected = i == model.selected();
    const int color = selected ? kWhite : kBlack;
    Item it;
    item(i, it, ctx);
    if (selected) epd().fillRoundRect(4, top + 1, kW - 14, kRowH - 2, 7, kBlack);
    int x = kMargin + 4;
    int end = kW - 16;
    if (it.current) {
      epd().fillCircle(x + 1, top + 8, 2, color);
      x += 8;
    }
    if (it.chevron) {
      chevron(end - 4, top + 8, color);
      end -= 12;
    }
    if (it.detail[0]) {
      const int dw = width(ui_small, it.detail);
      text(ui_small, it.detail, end - dw, top + kRowBaseline, dw, color);
      end -= dw + 10;
    }
    text(ui_body, it.label, x, top + kRowBaseline, end - x, color);
  }
  if (model.count() == 0) {
    text(ui_body, "Nothing here", 0, kRowsTop + 2 * kRowH, kW, kBlack, Align::Center);
  }
  scrollbar(model);
  footer(left, right);
}

void details(const char *title, const char *const *labels, const char *const *values, int count,
             Hint left, Hint right, int batteryPct) {
  display::clear();
  header(title, batteryPct);
  constexpr int kDetailRowH = 14;
  for (int i = 0; i < count && kRowsTop + (i + 1) * kDetailRowH <= kFooterTop; i++) {
    const int baseline = kRowsTop + i * kDetailRowH + 10;
    const int lw = text(ui_small, labels[i], kMargin, baseline, 90, kBlack);
    const int maxW = kW - 2 * kMargin - lw - 10;
    text(ui_body, values[i], kW - kMargin - maxW, baseline, maxW, kBlack, Align::Right);
  }
  footer(left, right);
}

void valueEditor(const char *title, const char *value, const char *caption, int percent, Hint left,
                 Hint right, int batteryPct) {
  display::clear();
  header(title, batteryPct);
  text(ui_display, value, 0, 52, kW, kBlack, Align::Center);
  const int x = 24, w = kW - 48, y = 62;
  epd().drawRoundRect(x, y, w, 9, 4, kBlack);
  const int pct = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
  const int fill = (w - 4) * pct / 100;
  if (fill >= 4) epd().fillRoundRect(x + 2, y + 2, fill, 5, 2, kBlack);
  text(ui_small, caption, 0, 88, kW, kBlack, Align::Center);
  footer(left, right);
}

void message(Icon which, const char *headline, const char *line1, const char *line2,
             const char *hint) {
  display::clear();
  icon(which, kW / 2, 8);
  text(ui_title, headline, kMargin, 52, kW - 2 * kMargin, kBlack, Align::Center);
  text(ui_body, line1, kMargin, 70, kW - 2 * kMargin, kBlack, Align::Center);
  text(ui_body, line2, kMargin, 85, kW - 2 * kMargin, kBlack, Align::Center);
  text(ui_small, hint, kMargin, kH - 6, kW - 2 * kMargin, kBlack, Align::Center);
}

void sleepScreen(const char *title, const char *author, int percentRead, int batteryPct) {
  display::clear();
  battery(kW - kMargin, 14, batteryPct, kBlack);

  constexpr int kLineH = 21;
  char lines[2][96];
  epd().setFont(ui_display);
  const int n = wrapText(title && *title ? title : "T5 Reader", kW - 40, measure, nullptr,
                         kEllipsis, lines[0], sizeof(lines[0]), 2);
  const bool hasAuthor = author && *author;
  const bool hasProgress = percentRead >= 0;
  const int blockH = n * kLineH + (hasAuthor ? 18 : 0) + (hasProgress ? 16 : 0);
  int y = 22 + (78 - blockH) / 2;
  for (int i = 0; i < n; i++, y += kLineH) {
    text(ui_display, lines[i], 0, y + 15, kW, kBlack, Align::Center);
  }
  if (hasAuthor) {
    text(ui_body, author, kMargin, y + 13, kW - 2 * kMargin, kBlack, Align::Center);
    y += 18;
  }
  if (hasProgress) {
    char pct[12];
    snprintf(pct, sizeof(pct), "%d%%", percentRead);
    const int pw = width(ui_small, pct);
    constexpr int kBarW = 70;
    const int x = (kW - kBarW - 6 - pw) / 2;
    epd().drawRoundRect(x, y + 7, kBarW, 5, 2, kBlack);
    const int fill = (kBarW - 2) * percentRead / 100;
    if (fill > 0) epd().fillRect(x + 1, y + 8, fill, 3, kBlack);
    text(ui_small, pct, x + kBarW + 6, y + 13, pw, kBlack);
  }

  const char *hint = "Sleeping  \xC2\xB7  press any button";
  const int hw = width(ui_small, hint);
  const int hx = (kW - hw - 12) / 2;
  moon(hx + 4, kH - 10);
  text(ui_small, hint, hx + 12, kH - 6, hw, kBlack);
}

}  // namespace ui
