// Screens drawn into the display framebuffer (call display::show() afterwards).
//
// Designed for the 250x122 T5-2.13, Ubuntu throughout; other boards draw the same
// design at kUiScale (board header) with their own font set, lists get as many
// rows as fit, and full-screen layouts are centred vertically.
//   reading  book page, status bar in the bottom 14 rows: a thin reading-progress
//            line, chapter left, page/percentage and battery right
//   lists    title + hairline, rows of 17 px with a rounded selection pill,
//            right-aligned values, chevrons, slim scrollbar, key-chip footer
//   messages icon, headline, body: no chrome
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "menu.h"

namespace ui {

// List rows that fit on the screen (5 on the T5-2.13).
int menuRows();

// One list row: label left, optional detail right, optional chevron.
struct Item {
  char label[96] = "";
  char detail[32] = "";
  bool chevron = false;  // opens another screen
  bool current = false;  // dot before the label (current chapter / book)
};
using ItemFn = void (*)(int index, Item &item, void *ctx);

// Footer hint: [KEY] action (hold) holdAction, for the NEXT or MENU role.
// The chips name the board's gestures: NEXT / MENU and "hold" with two buttons,
// TAP / HOLD and "2x" / "long" with one (see input.h). Touch boards draw the
// actions as buttons instead ([open] (back)), tappable via hit().
// kArrowDown / kArrowUp as an action draw an arrow instead of text.
constexpr const char *kArrowDown = "\xE2\x96\xBC";  // U+25BC
constexpr const char *kArrowUp = "\xE2\x96\xB2";    // U+25B2
enum class Button : uint8_t { None, Next, Menu };
struct Hint {
  Button button;
  const char *action;
  const char *holdAction;  // long press (one button: double tap / long hold); nullptr if none
};

enum class Icon : uint8_t { Card, Book, Battery, Warning };

// Touch boards: what is under (x, y) on the screen drawn last (rows, footer
// buttons, the Go to position bar). Kind None: nothing.
struct Hit {
  enum Kind : uint8_t { None, Chip, Row, Bar };
  Kind kind = None;
  Button button = Button::None;  // Chip: the role it stands for
  bool second = false;           // Chip: the second (long-press) action
  int row = -1;                  // Row: list index
  int percent = 0;               // Bar: position along the bar
};
Hit hit(int x, int y);

// Status bar over the bottom rows of a book page. batteryPct < 0 = unknown.
void statusBar(const char *chapter, uint32_t page, uint32_t pageCount, int batteryPct);

void menu(const char *title, const ListModel &model, ItemFn item, void *ctx, Hint left,
          Hint right, int batteryPct);

// Label/value rows (About).
void details(const char *title, const char *const *labels, const char *const *values, int count,
             Hint left, Hint right, int batteryPct);

// Large value with a progress bar and a caption (Go to position).
void valueEditor(const char *title, const char *value, const char *caption, int percent, Hint left,
                 Hint right, int batteryPct);

// Full-screen state: icon, headline, up to two lines, small hint at the bottom.
void message(Icon icon, const char *headline, const char *line1, const char *line2,
             const char *hint);
// Same, with key chips at the bottom instead of a hint.
void message(Icon icon, const char *headline, const char *line1, const char *line2, Hint left,
             Hint right);

void sleepScreen(const char *title, const char *author, int percentRead, int batteryPct);

// "Press any button to try again", in the board's words.
const char *retryHint();

// Wi-Fi transfer: QR code left, step + instructions right.
struct WifiView {
  const char *qr;  // QR payload (Wi-Fi join string or page URL)
  int step;        // 1 = join the network, 2 = open the page
  const char *headline;
  const char *label1;
  const char *value1;
  const char *label2;
  const char *value2;
  const char *event;  // last upload/delete, or ""
};
void wifiScreen(const WifiView &v, Hint left, Hint right, int batteryPct);

}  // namespace ui
