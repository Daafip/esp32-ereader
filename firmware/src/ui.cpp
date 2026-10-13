#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "display.h"
#include "fonts/ui_body.h"
#include "fonts/ui_body_2x.h"
#include "fonts/ui_display.h"
#include "fonts/ui_display_2x.h"
#include "fonts/ui_small.h"
#include "fonts/ui_small_2x.h"
#include "fonts/ui_title.h"
#include "fonts/ui_title_2x.h"
#include "qrcodegen.h"
#include "text_util.h"

namespace ui {
namespace {

// Every length below is in pixels of the 250x122 design, times S.
constexpr int S = kUiScale;
static_assert(S == 1 || S == 2, "UI fonts exist for scale 1 and 2 (tools/make_ui_fonts.sh)");
constexpr int kW = kPageW;
constexpr int kH = kPageH;
constexpr int kMargin = 8 * S;
constexpr int kHeaderBaseline = 14 * S;
constexpr int kHeaderRule = 19 * S;
constexpr int kRowsTop = 22 * S;
// Touch boards: rows and footer buttons big enough for a finger (about 6 mm).
constexpr bool T = kHasTouch;
constexpr int kRowH = (T ? 28 : 17) * S;
constexpr int kRowBaseline = (T ? 19 : 12) * S;
constexpr int kRowMid = (T ? 14 : 8) * S;  // centre line for dot and chevron
constexpr int kFooterTop = kH - (T ? 30 : 13) * S;
constexpr int kChipH = (T ? 26 : 12) * S;
// Full-screen layouts (messages, sleep, Wi-Fi, value editor) are laid out in a
// band of the design's 122 rows, centred on taller screens.
constexpr int kBandH = 122 * S;
constexpr int kBandTop = (kH - kBandH) / 2;  // 0 on the T5-2.13
const char *const kEllipsis = "\xE2\x80\xA6";
constexpr int kBlack = BBEP_BLACK;
constexpr int kWhite = BBEP_WHITE;

const uint8_t *const kSmall = S == 2 ? ui_small_2x : ui_small;
const uint8_t *const kBody = S == 2 ? ui_body_2x : ui_body;
const uint8_t *const kTitle = S == 2 ? ui_title_2x : ui_title;
const uint8_t *const kDisplay = S == 2 ? ui_display_2x : ui_display;

enum class Align { Left, Center, Right };

BBEPAPER &epd() { return display::epd(); }

// --- touch targets: what was drawn where, for hit() ------------------------------

struct Target {
  int16_t x, y, w, h;
  Hit hit;
};
Target targets[48];
int targetCount = 0;

void addTarget(int x, int y, int w, int h, const Hit &what) {
  if (!kHasTouch || targetCount >= int(sizeof(targets) / sizeof(targets[0]))) return;
  targets[targetCount++] = {int16_t(x), int16_t(y), int16_t(w), int16_t(h), what};
}

// Every screen starts on white with no targets.
void beginScreen() {
  display::clear();
  targetCount = 0;
}

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

// --- scaled strokes: a pen of S x S pixels ------------------------------------------

void dot(int x, int y, int color) { epd().fillRect(x, y, S, S, color); }

// Horizontal run from x0 to the pen at x1 (both ends included).
void hline(int x0, int x1, int y, int color) { epd().fillRect(x0, y, x1 - x0 + S, S, color); }

// Rounded outline `thickness` pixels wide; the radius shrinks inwards.
void outline(int x, int y, int w, int h, int r, int color, int thickness = S) {
  for (int i = 0; i < thickness; i++) {
    epd().drawRoundRect(x + i, y + i, w - 2 * i, h - 2 * i, r > i ? r - i : 0, color);
  }
}

// --- small vector icons ---------------------------------------------------------

void batteryIcon(int x, int y, int pct, int color) {  // 16 x 8
  outline(x, y, 14 * S, 8 * S, 2 * S, color);
  epd().fillRect(x + 14 * S, y + 2 * S, 2 * S, 4 * S, color);
  const int fill = pct <= 0 ? 0 : (pct * 10 + 50) / 100;
  if (fill > 0) epd().fillRect(x + 2 * S, y + 2 * S, fill * S, 4 * S, color);
}

// Battery percentage and icon, right-aligned at `right`. Returns the left edge.
int battery(int right, int baseline, int pct, int color) {
  if (pct < 0) return right;
  batteryIcon(right - 16 * S, baseline - 8 * S, pct, color);
  char label[12];
  snprintf(label, sizeof(label), "%d%%", pct);
  const int w = width(kSmall, label);
  text(kSmall, label, right - 16 * S - 4 * S - w, baseline, w, color);
  return right - 16 * S - 4 * S - w;
}

void chevron(int x, int cy, int color) {  // 5 x 7, two pixels thick
  for (int dx = 0; dx < 2 * S; dx++) {
    epd().drawLine(x + dx, cy - 3 * S, x + dx + 3 * S, cy, color);
    epd().drawLine(x + dx + 3 * S, cy, x + dx, cy + 3 * S, color);
  }
}

void moon(int cx, int cy) {
  epd().fillCircle(cx, cy, 4 * S, kBlack);
  epd().fillCircle(cx + 2 * S, cy - 2 * S, 3 * S, kWhite);
}

void icon(Icon which, int cx, int top) {  // about 28 x 26
  BBEPAPER &d = epd();
  switch (which) {
    case Icon::Card: {  // SD card: rounded body, cut corner, contacts
      const int x = cx - 10 * S, y = top;
      outline(x, y, 20 * S, 26 * S, 3 * S, kBlack, 2 * S);
      d.fillRect(x, y, 7 * S, 7 * S, kWhite);
      for (int i = 0; i < 2 * S; i++) d.drawLine(x + i, y + 6 * S, x + 6 * S, y + i, kBlack);
      for (int i = 0; i < 4; i++) d.fillRect(x + 7 * S + i * 3 * S, y + 3 * S, 2 * S, 5 * S, kBlack);
      break;
    }
    case Icon::Book: {  // closed book with spine and two title lines
      const int x = cx - 10 * S, y = top;
      outline(x, y, 20 * S, 26 * S, 2 * S, kBlack);
      d.fillRect(x, y, 4 * S, 26 * S, kBlack);
      hline(x + 8 * S, x + 16 * S, y + 7 * S, kBlack);
      hline(x + 8 * S, x + 14 * S, y + 10 * S, kBlack);
      break;
    }
    case Icon::Battery: {  // large, nearly empty
      const int x = cx - 16 * S, y = top + 6 * S;
      outline(x, y, 29 * S, 15 * S, 3 * S, kBlack, 2 * S);
      d.fillRect(x + 29 * S, y + 4 * S, 3 * S, 7 * S, kBlack);
      d.fillRect(x + 4 * S, y + 4 * S, 4 * S, 7 * S, kBlack);
      break;
    }
    case Icon::Warning: {  // triangle with an exclamation mark
      for (int t = 0; t < 2 * S; t++) {
        const int b = top + 24 * S - t;
        d.drawLine(cx, top + t, cx - 13 * S + t, b, kBlack);
        d.drawLine(cx, top + t, cx + 13 * S - t, b, kBlack);
        d.drawLine(cx - 13 * S + t, b, cx + 13 * S - t, b, kBlack);
      }
      d.fillRect(cx - S, top + 8 * S, 3 * S, 9 * S, kBlack);
      d.fillRect(cx - S, top + 19 * S, 3 * S, 2 * S, kBlack);
      break;
    }
  }
}

// --- chrome -------------------------------------------------------------------------

void header(const char *title, int batteryPct) {
  const int left = battery(kW - kMargin, kHeaderBaseline - S, batteryPct, kBlack);
  text(kTitle, title, kMargin, kHeaderBaseline, left - kMargin - 10 * S, kBlack);
  hline(kMargin, kW - kMargin - S, kHeaderRule, kBlack);
}

// Footer arrow (7 x 4), vertically centred on the chips.
void arrow(int x, bool up, int color = kBlack) {
  const int cy = kFooterTop + kChipH / 2;
  for (int i = 0; i < 4; i++) {
    const int y = up ? cy - 2 * S + i * S : cy + S - i * S;
    epd().fillRect(x + 3 * S - i * S, y, (2 * i + 1) * S, S, color);
  }
}

// Touch boards: the chip is the button. Filled for the short-press action,
// outlined for the second one; the label is the action (text or an arrow).
int touchChip(const char *label, bool filled, int x, bool draw, Button role, bool second) {
  const bool isArrow = label == kArrowDown || label == kArrowUp;
  const int lw = isArrow ? 7 * S : width(kBody, label);
  int w = lw + 24 * S;
  if (w < 44 * S) w = 44 * S;
  if (draw) {
    if (filled) {
      epd().fillRoundRect(x, kFooterTop, w, kChipH, kChipH / 2, kBlack);
    } else {
      outline(x, kFooterTop, w, kChipH, kChipH / 2, kBlack);
    }
    if (isArrow) {
      arrow(x + (w - 7 * S) / 2, label == kArrowUp, filled ? kWhite : kBlack);
    } else {
      text(kBody, label, x, kFooterTop + 17 * S, w, filled ? kWhite : kBlack, Align::Center);
    }
    Hit h;
    h.kind = Hit::Chip;
    h.button = role;
    h.second = second;
    addTarget(x, kFooterTop - 4 * S, w, kH - kFooterTop + 4 * S, h);
  }
  return w;
}

// A key chip: filled (the button) or outlined (the second gesture), then a label
// (text or an arrow). Returns the width; draw = false only measures.
int chip(const char *key, const char *label, bool filled, int x, bool draw) {
  const bool isArrow = label == kArrowDown || label == kArrowUp;
  const int kw = width(kSmall, key);
  const int lw = isArrow ? 7 * S : (label ? width(kSmall, label) : 0);
  if (draw) {
    if (filled) {
      epd().fillRoundRect(x, kFooterTop, kw + 10 * S, kChipH, 5 * S, kBlack);
    } else {
      outline(x, kFooterTop, kw + 10 * S, kChipH, 5 * S, kBlack);
    }
    text(kSmall, key, x + 5 * S, kFooterTop + 9 * S, kw, filled ? kWhite : kBlack);
    if (isArrow) {
      arrow(x + kw + 14 * S, label == kArrowUp);
    } else if (label) {
      text(kSmall, label, x + kw + 14 * S, kFooterTop + 9 * S, lw, kBlack);
    }
  }
  return kw + 10 * S + (lw ? 4 * S + lw : 0);
}

// Chip texts for a role: the button (or gesture) and its second gesture.
const char *keyName(Button b) {
  if (kSingleButton) return b == Button::Next ? "TAP" : "HOLD";
  return b == Button::Next ? "NEXT" : "MENU";
}

const char *secondName(Button b) {
  if (kSingleButton) return b == Button::Next ? "2\xC3\x97" : "long";  // U+00D7
  return "hold";
}

// "[KEY] action (hold) holdAction"; returns the width. Touch: [action] (holdAction).
int hintGroup(const Hint &h, int x, bool draw) {
  if (h.button == Button::None) return 0;
  if (kHasTouch) {
    int w = touchChip(h.action, true, x, draw, h.button, false);
    if (h.holdAction) w += 6 * S + touchChip(h.holdAction, false, x + w + 6 * S, draw, h.button, true);
    return w;
  }
  int w = chip(keyName(h.button), h.action, true, x, draw);
  if (h.holdAction) w += 7 * S + chip(secondName(h.button), h.holdAction, false, x + w + 7 * S, draw);
  return w;
}

void footer(Hint left, Hint right) {
  // Too wide for one row? Drop the second-gesture chips, right group first.
  const int room = kW - 2 * kMargin - 8 * S;
  if (hintGroup(left, 0, false) + hintGroup(right, 0, false) > room) right.holdAction = nullptr;
  if (hintGroup(left, 0, false) + hintGroup(right, 0, false) > room) left.holdAction = nullptr;
  hintGroup(left, kMargin, true);
  const int w = hintGroup(right, 0, false);
  if (w) hintGroup(right, kW - kMargin - w, true);
}

void scrollbar(const ListModel &m) {
  if (m.count() <= m.visibleRows()) return;
  const int x = kW - 4 * S;
  const int trackH = m.visibleRows() * kRowH;
  for (int y = kRowsTop; y < kRowsTop + trackH; y += 2 * S) dot(x, y, kBlack);
  int thumbH = trackH * m.visibleRows() / m.count();
  if (thumbH < 8 * S) thumbH = 8 * S;
  const int range = m.count() - m.visibleRows();
  const int y = kRowsTop + (trackH - thumbH) * m.first() / range;
  epd().fillRoundRect(x - S, y, 3 * S, thumbH, S, kBlack);
}

}  // namespace

int menuRows() { return (kFooterTop - kRowsTop) / kRowH; }

Hit hit(int x, int y) {
  for (int i = 0; i < targetCount; i++) {
    const Target &t = targets[i];
    if (x < t.x || y < t.y || x >= t.x + t.w || y >= t.y + t.h) continue;
    Hit h = t.hit;
    if (h.kind == Hit::Bar) {
      h.percent = t.w > 1 ? (x - t.x) * 100 / (t.w - 1) : 0;
      if (h.percent > 100) h.percent = 100;
    }
    return h;
  }
  return {};
}

void statusBar(const char *chapter, uint32_t page, uint32_t pageCount, int batteryPct) {
  targetCount = 0;  // the reading screen: tap zones, no targets
  const int top = kH - kStatusBarH;
  const int baseline = kH - 3 * S;
  epd().fillRect(0, top, kW, kStatusBarH, kWhite);
  // reading progress: dotted track, solid fill
  const int x0 = 4 * S, x1 = kW - 5 * S;
  for (int x = x0; x <= x1; x += 2 * S) dot(x, top + S, kBlack);
  if (pageCount) {
    const int fill = int(uint64_t(x1 - x0 + S) * (page + 1) / pageCount);
    if (fill > 0) epd().fillRect(x0, top, fill, 2 * S, kBlack);
  }
  int right = x1 + S;
  if (batteryPct >= 0) {
    batteryIcon(right - 16 * S, baseline - 7 * S, batteryPct, kBlack);
    right -= 16 * S + 7 * S;
  }
  char progress[32];
  formatProgress(page, pageCount, progress, sizeof(progress));
  const int pw = width(kSmall, progress);
  text(kSmall, progress, right - pw, baseline, pw, kBlack);
  text(kSmall, chapter, x0, baseline, right - pw - 12 * S - x0, kBlack);
}

void menu(const char *title, const ListModel &model, ItemFn item, void *ctx, Hint left,
          Hint right, int batteryPct) {
  beginScreen();
  header(title, batteryPct);
  for (int r = 0; r < model.visibleRows() && model.first() + r < model.count(); r++) {
    const int i = model.first() + r;
    const int top = kRowsTop + r * kRowH;
    const bool selected = i == model.selected();
    const int color = selected ? kWhite : kBlack;
    Item it;
    item(i, it, ctx);
    if (selected) {
      epd().fillRoundRect(4 * S, top + S, kW - 14 * S, kRowH - 2 * S, (T ? 10 : 7) * S, kBlack);
    }
    Hit row;
    row.kind = Hit::Row;
    row.row = i;
    addTarget(0, top, kW, kRowH, row);
    int x = kMargin + 4 * S;
    int end = kW - 16 * S;
    if (it.current) {
      epd().fillCircle(x + S, top + kRowMid, 2 * S, color);
      x += 8 * S;
    }
    if (it.chevron) {
      chevron(end - 4 * S, top + kRowMid, color);
      end -= 12 * S;
    }
    if (it.detail[0]) {
      const int dw = width(kSmall, it.detail);
      text(kSmall, it.detail, end - dw, top + kRowBaseline, dw, color);
      end -= dw + 10 * S;
    }
    text(kBody, it.label, x, top + kRowBaseline, end - x, color);
  }
  if (model.count() == 0) {
    text(kBody, "Nothing here", 0, kRowsTop + 2 * kRowH, kW, kBlack, Align::Center);
  }
  scrollbar(model);
  footer(left, right);
}

void details(const char *title, const char *const *labels, const char *const *values, int count,
             Hint left, Hint right, int batteryPct) {
  beginScreen();
  header(title, batteryPct);
  constexpr int kDetailRowH = 14 * S;
  for (int i = 0; i < count && kRowsTop + (i + 1) * kDetailRowH <= kFooterTop; i++) {
    const int baseline = kRowsTop + i * kDetailRowH + 10 * S;
    const int lw = text(kSmall, labels[i], kMargin, baseline, 90 * S, kBlack);
    const int maxW = kW - 2 * kMargin - lw - 10 * S;
    text(kBody, values[i], kW - kMargin - maxW, baseline, maxW, kBlack, Align::Right);
  }
  footer(left, right);
}

void valueEditor(const char *title, const char *value, const char *caption, int percent, Hint left,
                 Hint right, int batteryPct) {
  beginScreen();
  header(title, batteryPct);
  text(kDisplay, value, 0, kBandTop + 52 * S, kW, kBlack, Align::Center);
  const int x = 24 * S, w = kW - 48 * S, y = kBandTop + 62 * S;
  outline(x, y, w, 9 * S, 4 * S, kBlack);
  Hit bar;
  bar.kind = Hit::Bar;
  addTarget(x, y - 12 * S, w, 33 * S, bar);  // tap anywhere along the bar
  const int pct = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
  const int fill = (w - 4 * S) * pct / 100;
  if (fill >= 4 * S) epd().fillRoundRect(x + 2 * S, y + 2 * S, fill, 5 * S, 2 * S, kBlack);
  text(kSmall, caption, 0, kBandTop + 88 * S, kW, kBlack, Align::Center);
  footer(left, right);
}

namespace {
void messageBody(Icon which, const char *headline, const char *line1, const char *line2) {
  beginScreen();
  icon(which, kW / 2, kBandTop + 8 * S);
  text(kTitle, headline, kMargin, kBandTop + 52 * S, kW - 2 * kMargin, kBlack, Align::Center);
  text(kBody, line1, kMargin, kBandTop + 70 * S, kW - 2 * kMargin, kBlack, Align::Center);
  text(kBody, line2, kMargin, kBandTop + 85 * S, kW - 2 * kMargin, kBlack, Align::Center);
}
}  // namespace

void message(Icon which, const char *headline, const char *line1, const char *line2,
             const char *hint) {
  messageBody(which, headline, line1, line2);
  text(kSmall, hint, kMargin, kH - 6 * S, kW - 2 * kMargin, kBlack, Align::Center);
}

void message(Icon which, const char *headline, const char *line1, const char *line2, Hint left,
             Hint right) {
  messageBody(which, headline, line1, line2);
  footer(left, right);
}

void sleepScreen(const char *title, const char *author, int percentRead, int batteryPct) {
  beginScreen();
  battery(kW - kMargin, 14 * S, batteryPct, kBlack);

  constexpr int kLineH = 21 * S;
  char lines[2][96];
  epd().setFont(kDisplay);
  const int n = wrapText(title && *title ? title : "T5 Reader", kW - 40 * S, measure, nullptr,
                         kEllipsis, lines[0], sizeof(lines[0]), 2);
  const bool hasAuthor = author && *author;
  const bool hasProgress = percentRead >= 0;
  const int blockH = n * kLineH + (hasAuthor ? 18 * S : 0) + (hasProgress ? 16 * S : 0);
  int y = kBandTop + 22 * S + (78 * S - blockH) / 2;
  for (int i = 0; i < n; i++, y += kLineH) {
    text(kDisplay, lines[i], 0, y + 15 * S, kW, kBlack, Align::Center);
  }
  if (hasAuthor) {
    text(kBody, author, kMargin, y + 13 * S, kW - 2 * kMargin, kBlack, Align::Center);
    y += 18 * S;
  }
  if (hasProgress) {
    char pct[12];
    snprintf(pct, sizeof(pct), "%d%%", percentRead);
    const int pw = width(kSmall, pct);
    constexpr int kBarW = 70 * S;
    const int x = (kW - kBarW - 6 * S - pw) / 2;
    outline(x, y + 7 * S, kBarW, 5 * S, 2 * S, kBlack);
    const int fill = (kBarW - 2 * S) * percentRead / 100;
    if (fill > 0) epd().fillRect(x + S, y + 8 * S, fill, 3 * S, kBlack);
    text(kSmall, pct, x + kBarW + 6 * S, y + 13 * S, pw, kBlack);
  }

  const char *hint = kSleepHint;
  const int hw = width(kSmall, hint);
  const int hx = (kW - hw - 12 * S) / 2;
  moon(hx + 4 * S, kH - 10 * S);
  text(kSmall, hint, hx + 12 * S, kH - 6 * S, hw, kBlack);
}

const char *retryHint() {
  if (kHasTouch) return "Tap the screen to try again";
  return kSingleButton ? "Press the button to try again" : "Press any button to try again";
}

void wifiScreen(const WifiView &v, Hint left, Hint right, int batteryPct) {
  beginScreen();
  // QR code: up to version 6 (41 modules), as large as fits in 104 px with a
  // 2-module quiet zone, centred between the top of the band and the footer.
  uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
  uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
  int qrRight = kMargin;
  if (v.qr && qrcodegen_encodeText(v.qr, tmp, qr, qrcodegen_Ecc_LOW, 1, 6, qrcodegen_Mask_AUTO,
                                   true)) {
    const int size = qrcodegen_getSize(qr);
    const int scale = 104 * S / (size + 4);
    const int px = (size + 4) * scale;
    const int x0 = 2 * S + 2 * scale;
    const int y0 = kBandTop + (kBandH - 13 * S - px) / 2 + 2 * scale;
    for (int y = 0; y < size; y++) {
      for (int x = 0; x < size; x++) {
        if (qrcodegen_getModule(qr, x, y)) {
          epd().fillRect(x0 + x * scale, y0 + y * scale, scale, scale, kBlack);
        }
      }
    }
    qrRight = 2 * S + px + 6 * S;
  }
  const int x = qrRight;
  const int w = kW - x - kMargin;
  const int batteryLeft = battery(kW - kMargin, 12 * S, batteryPct, kBlack);
  const int top = kBandTop;
  if (v.step > 0) {
    char step[24];
    snprintf(step, sizeof(step), "Step %d of 2", v.step);
    const int stepRight = top > 0 ? kW - kMargin : batteryLeft - 6 * S;  // battery above the band?
    text(kSmall, step, x, top + 12 * S, stepRight - x, kBlack);
  }
  text(kTitle, v.headline, x, top + 30 * S, w, kBlack);
  text(kSmall, v.label1, x, top + 46 * S, w, kBlack);
  text(kTitle, v.value1, x, top + 61 * S, w, kBlack);
  text(kSmall, v.label2, x, top + 76 * S, w, kBlack);
  text(kTitle, v.value2, x, top + 91 * S, w, kBlack);
  text(kSmall, v.event, x, top + 104 * S, w, kBlack);
  footer(left, right);
}

}  // namespace ui
