// Hardware test for the 7.5" reader: DFRobot FireBeetle 2 ESP32-C6, Waveshare
// 7.5" e-Paper HAT V2, three Gravity buttons, and a microSD breakout if fitted.
// Pins: include/boards/firebeetle_75.h. Run it before the reader is ported, to
// check what the port will rely on (docs/checkpoints.md):
//   pixi run fw-flash fb75_test, then pixi run fw-monitor
//
// At power-on: battery voltage, button idle levels, bb_epaper's buffer layout vs
// the .t5b page layout (no panel needed), SD card (mount, random 48 KB page reads
// as the reader does them), and the controller family from BUSY after a reset
// (UC81xx: high = ready). Nothing is sent to the panel unless it answers as one.
// Then one step per press (step 1 starts by itself):
//   1  full refresh of a test screen: which edge is up, borders, mirroring, text,
//      dither patterns
//   2  fast refresh
//   3  5 partial refreshes (counter)
//   4  button log: press, release, duration (until a key is typed, or RST)
// The chip stays awake: deep sleep and the button wake are left for the reader port.
//
// Controls: NEXT / 'n' next step, PREV / 'r' repeat the step, HOME / 'p' other
// panel type (back to step 1), 's' summary, 'c' clear the panel, 'f' drive the
// panel even though BUSY doesn't look like a UC81xx, 'b' BUSY trace (below).
//
// BUSY trace ('b'), for a panel that stays busy: the BUSY line with the internal
// pull-up and pull-down (driven or floating?), then the bring-up command by
// command over plain SPI at 4 MHz, BUSY timed after each: pass A with bb_epaper's
// init, pass B with Waveshare's (adds the booster soft-start, 0x06). Where BUSY
// stays low too long it also tries status reads (0x71), which Waveshare's and
// ESPHome's drivers send while they wait. Blind refresh ('w'): the same with fixed
// waits instead of BUSY, so the screen shows whether the panel works at all.
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <bb_epaper.h>
#include <driver/rtc_io.h>
#include <esp_random.h>
#include <string.h>

#include <initializer_list>

#include "board.h"
#include "fonts/ui_body_2x.h"
#include "fonts/ui_display_2x.h"
#include "fonts/ui_small_2x.h"
#include "fonts/ui_title_2x.h"

namespace {

struct Candidate {
  int panel;
  const char *name;
  const char *note;
};

const Candidate kCandidates[] = {
    {EP75_800x480_GEN2, "EP75_800x480_GEN2", "Waveshare V2 (GEDY075-D2)"},
    {EP75_800x480, "EP75_800x480", "older init (GDEY075T7)"},
};
constexpr int kNumCandidates = sizeof(kCandidates) / sizeof(kCandidates[0]);
constexpr int kPartialSteps = 5;
constexpr int kButtons[] = {kBtnPrev, kBtnNext, kBtnMenu};
const char *const kButtonNames[] = {"PREV", "NEXT", "HOME"};

// Results, and what the glass shows.
struct Results {
  int candidate;
  bool busyOk;  // BUSY read high (ready) after a reset: a UC81xx
  bool layoutOk;
  int batteryMv;
  char buttonsIdle[4];  // per button: L / H at idle, without internal pulls
  int sdMb;             // -1 = no card
  int sdPageMs;         // average random 48 KB page read
  int fullMs, fastMs, partialMs;
  int shownCounter;     // the screen on the glass, drawn again as the "old" image
  char shownStatus[48];  // "" = unknown
};
Results res;

BBEPAPER epd;
// The panel buffer in the rotated (portrait) layout; also scratch for the SD test.
uint8_t fb[(kEpdNativeW / 8) * kEpdNativeH];
static_assert(sizeof(fb) >= size_t(kPageBytes), "framebuffer smaller than a page");
int step = 0;  // last step run, 0 = none
bool driveAnyway = false;
int lastDataMs = 0;

bool pressed(int pin) { return digitalRead(pin) == (kBtnActiveHigh ? HIGH : LOW); }

void printSummary() {
  Serial.printf(
      "SUMMARY panel=%s busy=%s layout=%s battery_mv=%d buttons_idle=%s sd_mb=%d sd_page_ms=%d "
      "full_ms=%d fast_ms=%d partial_ms=%d\n",
      kCandidates[res.candidate].name, res.busyOk ? "UC81xx" : "not-UC81xx",
      res.layoutOk ? "ok" : "BAD", res.batteryMv, res.buttonsIdle, res.sdMb, res.sdPageMs,
      res.fullMs, res.fastMs, res.partialMs);
}

// --- checks at power-on --------------------------------------------------------

void checkBattery() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(kBatAdc);
  const int pinMv = int(sum / 16);
  res.batteryMv = pinMv * kBatDividerNum;
  Serial.printf("BATTERY: GPIO%d %d mV x %d = %d mV%s. Compare with a multimeter on the cell.\n",
                kBatAdc, pinMv, kBatDividerNum, res.batteryMv,
                res.batteryMv < 2500 ? " (no cell? USB only)" : "");
}

// Idle levels without internal pulls: the module's own resistor decides.
void checkButtons() {
  for (int i = 0; i < 3; i++) {
    const int pin = kButtons[i];
    if (rtc_gpio_is_valid_gpio(gpio_num_t(pin))) rtc_gpio_deinit(gpio_num_t(pin));
    pinMode(pin, INPUT);
  }
  delay(5);
  for (int i = 0; i < 3; i++) {
    const int pin = kButtons[i];
    const int level = digitalRead(pin);
    res.buttonsIdle[i] = level ? 'H' : 'L';
    Serial.printf("BUTTON %s GPIO%d idle %s%s\n", kButtonNames[i], pin, level ? "HIGH" : "LOW",
                  pressed(pin) ? "  <-- reads as pressed: wiring? cable on the right pin?" : "  ok");
    pinMode(pin, kBtnActiveHigh ? INPUT_PULLDOWN : INPUT_PULLUP);
  }
  res.buttonsIdle[3] = '\0';
}

// The reader copies .t5b pages straight into bb_epaper's buffer, so it must be
// that layout: kPageStride bytes per row, MSB first, 1 = white (docs/t5b-format.md).
void checkLayout() {
  epd.setPanelType(kCandidates[res.candidate].panel);
  epd.setRotation(kEpdRotation);
  epd.setBuffer(fb);
  memset(fb, 0xFF, kPageBytes);
  epd.drawPixel(43, 300, BBEP_BLACK);
  epd.drawPixel(kPageW - 1, kPageH - 1, BBEP_BLACK);
  int changed = 0;
  for (int i = 0; i < kPageBytes; i++) changed += fb[i] != 0xFF;
  res.layoutOk = epd.width() == kPageW && epd.height() == kPageH && changed == 2 &&
                 fb[300 * kPageStride + 43 / 8] == uint8_t(~(0x80 >> (43 % 8))) &&
                 fb[(kPageH - 1) * kPageStride + (kPageW - 1) / 8] ==
                     uint8_t(~(0x80 >> ((kPageW - 1) % 8)));
  Serial.printf("LAYOUT: bb_epaper buffer %dx%d at rotation %d %s the .t5b page layout\n",
                epd.width(), epd.height(), kEpdRotation, res.layoutOk ? "matches" : "DOES NOT MATCH");
}

// The card shares the panel's bus (the C6 has one). The SD test runs first and
// starts the bus with MISO; bb_epaper's initIO() would start it without.
void checkSd() {
  res.sdMb = -1;
  res.sdPageMs = -1;
  pinMode(kEpdCs, OUTPUT);
  digitalWrite(kEpdCs, HIGH);  // the panel ignores the card's traffic
  SPI.begin(kSdSck, kSdMiso, kSdMosi, -1);
  const uint32_t speeds[] = {kSdSpiHz, 10000000, 4000000};
  uint32_t hz = 0;
  for (uint32_t s : speeds) {
    if (SD.begin(kSdCs, SPI, s)) {
      hz = s;
      break;
    }
    SD.end();
  }
  if (!hz) {
    Serial.printf("SD: no card (MISO GPIO%d, CS GPIO%d). Fine if the module isn't fitted yet; "
                  "the reader will need one.\n", kSdMiso, kSdCs);
    return;
  }
  res.sdMb = int(SD.cardSize() >> 20);
  Serial.printf("SD: %d MB, type %d, %u MHz\n", res.sdMb, int(SD.cardType()), unsigned(hz / 1000000));

  // A small "book" of 16 pages, then random page reads: one per page turn in the reader.
  constexpr int kPages = 16;
  const char *path = "/fb75test.bin";
  fs::File f = SD.open(path, FILE_WRITE);
  bool ok = bool(f);
  for (int p = 0; ok && p < kPages; p++) {
    memset(fb, p, kPageBytes);
    ok = f.write(fb, kPageBytes) == size_t(kPageBytes);
  }
  if (f) f.close();
  uint32_t total = 0;
  f = SD.open(path, FILE_READ);
  ok = ok && bool(f);
  for (int i = 0; ok && i < kPages; i++) {
    const int p = int(esp_random() % kPages);
    const uint32_t t0 = millis();
    ok = f.seek(uint32_t(p) * kPageBytes) && f.read(fb, kPageBytes) == size_t(kPageBytes);
    total += millis() - t0;
    ok = ok && fb[0] == p && fb[kPageBytes - 1] == p;
  }
  if (f) f.close();
  SD.remove(path);
  if (ok) res.sdPageMs = int(total / kPages);
  Serial.printf("SD: %s, page read %d ms on average (%d B)\n", ok ? "data ok" : "WRITE/READ FAILED",
                res.sdPageMs, kPageBytes);
}

// --- panel ------------------------------------------------------------------------

void resetPanel() {
  pinMode(kEpdCs, OUTPUT);
  digitalWrite(kEpdCs, HIGH);
  pinMode(kEpdRst, OUTPUT);
  digitalWrite(kEpdRst, HIGH);
  delay(20);
  digitalWrite(kEpdRst, LOW);
  delay(kEpdResetMs);
  digitalWrite(kEpdRst, HIGH);
  delay(20);
}

// UC81xx: BUSY high = ready. With the pull-down, an unconnected BUSY reads low too.
bool probeUc81xx() {
  pinMode(kEpdBusy, INPUT_PULLDOWN);
  resetPanel();
  Serial.print("BUSY after reset (ms:level):");
  int level = LOW;
  for (int t = 0; t <= 200; t += 10) {
    level = digitalRead(kEpdBusy);
    if (t % 50 == 0) Serial.printf(" %d:%d", t, level);
    delay(10);
  }
  Serial.println();
  pinMode(kEpdBusy, INPUT);
  return level == HIGH;
}

bool panelAllowed() {
  if (res.busyOk || driveAnyway) return true;
  Serial.println("Panel skipped: BUSY doesn't read high (ready) as on a UC81xx. Type 'b' for a\n"
                 "BUSY trace, or 'f' to drive the panel anyway.");
  return false;
}

// --- BUSY trace ('b') -------------------------------------------------------------

constexpr uint32_t kTraceSpiHz = 4000000;  // slow: rules out the wires
bool statusReadsHelped = false;

void rawCommand(uint8_t cmd, std::initializer_list<uint8_t> data = {}) {
  SPI.beginTransaction(SPISettings(kTraceSpiHz, MSBFIRST, SPI_MODE0));
  digitalWrite(kEpdDc, LOW);
  digitalWrite(kEpdCs, LOW);
  SPI.transfer(cmd);
  digitalWrite(kEpdCs, HIGH);
  digitalWrite(kEpdDc, HIGH);
  if (data.size()) {
    digitalWrite(kEpdCs, LOW);
    for (uint8_t b : data) SPI.transfer(b);
    digitalWrite(kEpdCs, HIGH);
  }
  SPI.endTransaction();
}

// fb as one RAM plane (0x10 old, 0x13 new), native layout.
void rawPlane(uint8_t cmd) {
  rawCommand(cmd);
  SPI.beginTransaction(SPISettings(kTraceSpiHz, MSBFIRST, SPI_MODE0));
  digitalWrite(kEpdCs, LOW);
  SPI.writeBytes(fb, kPageBytes);
  digitalWrite(kEpdCs, HIGH);
  SPI.endTransaction();
}

void rawFill(uint8_t cmd, uint8_t value) {
  memset(fb, value, kPageBytes);
  rawPlane(cmd);
}

void rawSetup() {
  SPI.begin(kSdSck, kSdMiso, kSdMosi, -1);  // as at start-up (no-op if running)
  pinMode(kEpdDc, OUTPUT);
  digitalWrite(kEpdDc, HIGH);
  pinMode(kSdCs, OUTPUT);
  digitalWrite(kSdCs, HIGH);  // the card stays out of it
}

// How long BUSY stays low after a command. Still low after limitMs: up to 2 s
// more with status reads (0x71). False if it never came back high.
bool watchBusy(const char *what, uint32_t limitMs) {
  const uint32_t t0 = millis();
  bool wentLow = false;
  while (millis() - t0 < limitMs) {
    const bool low = digitalRead(kEpdBusy) == LOW;
    if (low) wentLow = true;
    if (!low && (wentLow || millis() - t0 >= 20)) break;  // back high, or never low
    delayMicroseconds(250);
  }
  const unsigned ms = unsigned(millis() - t0);
  if (digitalRead(kEpdBusy) == HIGH) {
    if (wentLow) {
      Serial.printf("  %-28s busy %u ms\n", what, ms);
    } else {
      Serial.printf("  %-28s not busy\n", what);
    }
    return true;
  }
  const uint32_t t1 = millis();
  while (digitalRead(kEpdBusy) == LOW && millis() - t1 < 2000) {
    rawCommand(0x71);
    delay(20);
  }
  if (digitalRead(kEpdBusy) == HIGH) {
    statusReadsHelped = true;
    Serial.printf("  %-28s LOW for %u ms, high after status reads (0x71), %u ms more\n", what, ms,
                  unsigned(millis() - t1));
    return true;
  }
  Serial.printf("  %-28s STAYED LOW > %u ms, also with status reads\n", what, ms + 2000);
  return false;
}

// BUSY with the internal pull-up and with the pull-down: driven or floating?
const char *busyLine(const char *when) {
  pinMode(kEpdBusy, INPUT_PULLUP);
  delay(5);
  const int up = digitalRead(kEpdBusy);
  pinMode(kEpdBusy, INPUT_PULLDOWN);
  delay(5);
  const int down = digitalRead(kEpdBusy);
  pinMode(kEpdBusy, INPUT);
  const char *state = up && down     ? "high"
                      : !up && !down ? "low"
                      : up           ? "floating"
                                     : "odd";
  Serial.printf("BUSY line %s: pull-up %d, pull-down %d -> %s\n", when, up, down,
                up && down     ? "driven HIGH (ready)"
                : !up && !down ? "driven LOW (busy, in reset, or the panel unpowered)"
                : up           ? "FLOATING: nothing drives GPIO21"
                               : "odd");
  return state;
}

// One bring-up and a white full refresh. Returns the step BUSY got stuck at, or nullptr.
const char *tracePass(bool waveshare) {
  Serial.printf("pass %c: %s\n", waveshare ? 'B' : 'A',
                waveshare ? "Waveshare's init (with booster soft-start 0x06)"
                          : "bb_epaper's EP75_800x480_GEN2 init");
  const char *stuck = nullptr;
  auto step = [&](const char *name, uint8_t cmd, std::initializer_list<uint8_t> data,
                  uint32_t limitMs) {
    if (stuck) return;
    rawCommand(cmd, data);
    if (!watchBusy(name, limitMs)) stuck = name;
  };
  resetPanel();
  if (!watchBusy("reset", 1000)) stuck = "reset";
  step("power setting 0x01", 0x01, {0x07, 0x07, 0x3F, 0x3F}, 100);
  if (waveshare) step("booster soft-start 0x06", 0x06, {0x17, 0x17, 0x28, 0x17}, 100);
  step("power on 0x04", 0x04, {}, 5000);
  step("panel setting 0x00", 0x00, {0x1F}, 100);
  step("resolution 0x61", 0x61, {0x03, 0x20, 0x01, 0xE0}, 100);
  step("dual SPI off 0x15", 0x15, {0x00}, 100);
  if (waveshare) {
    step("VCOM/data interval 0x50", 0x50, {0x10, 0x07}, 100);
  } else {
    step("VCOM/data interval 0x50", 0x50, {0x21, 0x07}, 100);
  }
  step("TCON 0x60", 0x60, {0x22}, 100);
  if (!stuck) {
    rawFill(0x10, 0xFF);                     // white: Waveshare's polarity differs with
    rawFill(0x13, waveshare ? 0x00 : 0xFF);  // its VCOM/data setting
    if (!watchBusy("image 0x10 + 0x13", 100)) stuck = "image";
  }
  step("refresh 0x12", 0x12, {}, 20000);
  rawCommand(0x02);  // power off, also after a failure: don't leave the booster on
  watchBusy("power off 0x02", 2000);
  rawCommand(0x07, {0xA5});  // deep sleep
  Serial.printf("pass %c: %s%s\n", waveshare ? 'B' : 'A', stuck ? "STUCK at " : "ok",
                stuck ? stuck : "");
  return stuck;
}

void busyTrace() {
  Serial.printf("\n=== BUSY trace: GPIO%d, SPI %u MHz, reset %d ms ===\n", kEpdBusy,
                unsigned(kTraceSpiHz / 1000000), kEpdResetMs);
  rawSetup();
  const char *before = busyLine("before reset");
  resetPanel();
  delay(200);
  const char *after = busyLine("200 ms after reset");
  statusReadsHelped = false;
  const char *a = tracePass(false);
  const char *b = tracePass(true);
  Serial.printf("TRACE line_before=%s line_after=%s a=%s b=%s status_reads_needed=%s\n", before,
                after, a ? a : "ok", b ? b : "ok", statusReadsHelped ? "yes" : "no");
  if (!strcmp(after, "floating")) {
    Serial.println("Hint: nothing drives BUSY. Is the BUSY wire on GPIO21 (check the HAT's labels,\n"
                   "not just the colours)? 3.3 V on the HAT's VCC and PWR, GND shared? Panel\n"
                   "ribbon fully in and latched? (Without power or panel, BUSY floats.)");
  } else if (a && b && !strcmp(a, b) && !strcmp(a, "reset")) {
    Serial.println("Hint: busy before any command. HAT PWR at 3.3 V? Ribbon fully in, contacts\n"
                   "the right way round, latch closed?");
  } else if (a && b && !strcmp(a, "power on 0x04") && !strcmp(b, "power on 0x04")) {
    Serial.println("Hint: the booster doesn't start. Unplug, check the ribbon and the Display\n"
                   "Config switch (try the other position), plug in, run 'b' again. A weak USB\n"
                   "port or cable can do this too.");
  } else if (a && !b) {
    Serial.println("Hint: Waveshare's init works, bb_epaper's doesn't: the port will use Waveshare's.");
  } else if (!a && !b) {
    Serial.println("Hint: the panel works with plain commands here. Run step 1 again ('n' or 'r');\n"
                   "if it still times out, the difference is the SPI clock or bb_epaper.");
  }
}

// Blind refresh ('w'): Waveshare's init and a half black, half white image, with
// fixed waits instead of BUSY. The screen changes: panel and SPI work, only BUSY
// doesn't come back. It doesn't: the panel gets no power, or no commands.
void blindRefresh() {
  Serial.println("\n=== blind refresh: fixed waits, BUSY ignored. Watch the screen. ===");
  rawSetup();
  pinMode(kEpdBusy, INPUT_PULLUP);  // a floating line stays high: changes are real
  int last = digitalRead(kEpdBusy);
  int changes = 0;
  auto wait = [&](uint32_t ms) {
    for (const uint32_t t0 = millis(); millis() - t0 < ms;) {
      const int level = digitalRead(kEpdBusy);
      if (level != last) changes++;
      last = level;
      delay(1);
    }
  };
  resetPanel();
  wait(200);
  rawCommand(0x01, {0x07, 0x07, 0x3F, 0x3F});
  rawCommand(0x06, {0x17, 0x17, 0x28, 0x17});
  rawCommand(0x04);  // power on
  wait(500);
  rawCommand(0x00, {0x1F});
  rawCommand(0x61, {0x03, 0x20, 0x01, 0xE0});
  rawCommand(0x15, {0x00});
  rawCommand(0x50, {0x10, 0x07});
  rawCommand(0x60, {0x22});
  rawFill(0x10, 0xFF);
  memset(fb, 0xFF, kPageBytes / 2);  // one half of the new image one colour,
  memset(fb + kPageBytes / 2, 0x00, kPageBytes / 2);  // the other half the other
  rawPlane(0x13);
  rawCommand(0x12);  // refresh
  Serial.println("refresh sent: the screen should flash for a few seconds and end half black.");
  wait(8000);
  rawCommand(0x02);  // power off
  wait(1000);
  rawCommand(0x07, {0xA5});  // deep sleep
  pinMode(kEpdBusy, INPUT);
  Serial.printf("BLIND done: BUSY changed %d times (0 = never moved). Did the screen change?\n",
                changes);
}

// Panel up for one step: our reset (kEpdResetMs), then bb_epaper's init sequence.
// initIO() also sets the bus back to the panel's clock (the SD library leaves the
// card's) and leaves a bus that is already running (with MISO) as it is.
void beginPanel() {
  epd.setPanelType(kCandidates[res.candidate].panel);
  resetPanel();
  pinMode(kEpdBusy, INPUT);
  for (uint32_t t0 = millis(); digitalRead(kEpdBusy) == LOW && millis() - t0 < 1000;) delay(2);
  epd._bbep.is_awake = 1;  // just reset: no wake-up reset of bb_epaper's own (10 ms)
  epd.initIO(kEpdDc, kEpdRst, kEpdBusy, kEpdCs, kEpdMosi, kEpdSck, kEpdSpiHz);
  epd.setRotation(kEpdRotation);
  epd.setBuffer(fb);
}

// Refresh and our own wait: bb_epaper gives up after 5 s.
int refreshAndWait(int mode) {
  const uint32_t t0 = millis();
  epd.refresh(mode, false);
  delay(10);
  while (epd.isBusy() && millis() - t0 < kEpdBusyTimeoutMs) delay(5);
  const int ms = int(millis() - t0);
  if (ms >= int(kEpdBusyTimeoutMs)) Serial.println("TIMEOUT: BUSY stayed low ('b': BUSY trace)");
  return ms;
}

void text(const uint8_t *font, const char *s, int x, int baseline) {
  epd.setFont(font);
  epd.setTextColor(BBEP_BLACK, BBEP_TRANSPARENT);
  epd.drawString(s, x, baseline);
}

void checker(int x0, int y0, int size, int cell) {
  for (int y = 0; y < size; y += cell) {
    for (int x = 0; x < size; x += cell) {
      if ((x / cell + y / cell) & 1) epd.fillRect(x0 + x, y0 + y, cell, cell, BBEP_BLACK);
    }
  }
}

// The test screen: `counter` in the box bottom right, `status` says what happened.
void drawScreen(int counter, const char *status) {
  const int w = epd.width();
  const int h = epd.height();
  epd.fillScreen(BBEP_WHITE);
  // Borders on the outermost pixels and 3 px in: a missing line = rows or columns cut off.
  epd.drawRect(0, 0, w, h, BBEP_BLACK);
  epd.drawRect(3, 3, w - 6, h - 6, BBEP_BLACK);
  // Corners: top-left filled, the others hollow (mirroring, rotation).
  epd.fillRect(8, 8, 24, 24, BBEP_BLACK);
  epd.drawRect(w - 32, 8, 24, 24, BBEP_BLACK);
  epd.drawRect(8, h - 32, 24, 24, BBEP_BLACK);
  epd.drawRect(w - 32, h - 32, 24, 24, BBEP_BLACK);
  // Arrow at the top edge.
  for (int i = 0; i < 28; i++) epd.drawLine(w / 2 - i, 12 + i, w / 2 + i, 12 + i, BBEP_BLACK);
  epd.fillRect(w / 2 - 8, 40, 16, 30, BBEP_BLACK);
  text(ui_title_2x, "TOP", w / 2 + 40, 50);

  char s[96];
  text(ui_title_2x, "7.5\" reader test", 24, 120);
  snprintf(s, sizeof(s), "%s, %dx%d, rot %d", kCandidates[res.candidate].name, w, h,
           epd.getRotation());
  text(ui_small_2x, s, 24, 156);
  text(ui_body_2x, status, 24, 196);

  // Dither patterns (sleep pictures, covers): 1, 2 and 4 px checkerboards, black.
  text(ui_small_2x, "1 px", 24, 250);
  text(ui_small_2x, "2 px", 136, 250);
  text(ui_small_2x, "4 px", 248, 250);
  checker(24, 262, 96, 1);
  checker(136, 262, 96, 2);
  checker(248, 262, 96, 4);
  epd.fillRect(360, 262, 96, 96, BBEP_BLACK);

  // Text in the menu fonts at this density.
  text(ui_title_2x, "Chapter 1", 24, 420);
  text(ui_body_2x, "It was a bright cold day", 24, 460);
  text(ui_body_2x, "in April, and the clocks", 24, 494);
  text(ui_body_2x, "were striking thirteen.", 24, 528);
  text(ui_small_2x, "0123456789 AaBbCcDdEeFf .,:;!?", 24, 566);

  text(ui_small_2x, "NEXT  next step", 24, h - 130);
  text(ui_small_2x, "PREV  repeat", 24, h - 100);
  text(ui_small_2x, "HOME  other panel", 24, h - 70);
  if (counter >= 0) {
    epd.drawRect(w - 160, h - 150, 120, 100, BBEP_BLACK);
    snprintf(s, sizeof(s), "%02d", counter);
    text(ui_display_2x, s, w - 140, h - 80);
  }
}

// Draws a screen and refreshes. Partial: the screen on the glass goes to the
// controller's "old" RAM first (drawn again), the new one to "new".
int showScreen(int counter, const char *status, int mode) {
  if (mode == REFRESH_PARTIAL && !res.shownStatus[0]) {
    Serial.println("(what the glass shows is unknown: full refresh instead of partial)");
    mode = REFRESH_FULL;
  }
  const uint32_t t0 = millis();
  if (mode == REFRESH_PARTIAL) {
    drawScreen(res.shownCounter, res.shownStatus);
    epd.writePlane(PLANE_0_TO_1);
    drawScreen(counter, status);
    epd.writePlane(PLANE_0);
  } else {
    drawScreen(counter, status);
    epd.writePlane(PLANE_DUPLICATE);
  }
  lastDataMs = int(millis() - t0);
  const int ms = refreshAndWait(mode);
  res.shownCounter = counter;
  strlcpy(res.shownStatus, status, sizeof(res.shownStatus));
  return ms;
}

void clearPanel() {
  beginPanel();
  memset(fb, 0xFF, kPageBytes);
  epd.writePlane(PLANE_DUPLICATE);
  refreshAndWait(REFRESH_FULL);
  epd.sleep(DEEP_SLEEP);
  res.shownStatus[0] = '\0';
  Serial.println("cleared");
}

// --- steps ------------------------------------------------------------------------

void stepFull() {
  beginPanel();
  res.fullMs = showScreen(0, "1: full refresh", REFRESH_FULL);
  epd.sleep(LIGHT_SLEEP);
  Serial.printf("RESULT full: data %d ms, refresh %d ms\n", lastDataMs, res.fullMs);
  Serial.println("Judge: arrow at the edge that will be up (else rotation 270)? Filled square top\n"
                 "left (else mirrored)? Both borders complete on all four sides? Text sharp, white\n"
                 "background, checkerboards even?");
}

void stepFast() {
  beginPanel();
  res.fastMs = showScreen(0, "2: fast refresh", REFRESH_FAST);
  epd.sleep(LIGHT_SLEEP);
  Serial.printf("RESULT fast: data %d ms, refresh %d ms\n", lastDataMs, res.fastMs);
  Serial.println("Judge: a short flash, then a clean image without traces of step 1?");
}

void stepPartial() {
  beginPanel();
  int sum = 0;
  for (int i = 1; i <= kPartialSteps; i++) {
    const int ms = showScreen(i, "3: partial refresh", REFRESH_PARTIAL);
    Serial.printf("partial %d: data %d ms, refresh %d ms\n", i, lastDataMs, ms);
    sum += ms;
  }
  res.partialMs = sum / kPartialSteps;
  epd.sleep(LIGHT_SLEEP);
  Serial.printf("RESULT partial: refresh %d ms on average\n", res.partialMs);
  Serial.println("Judge: counter 1..5 without flashing, the rest unchanged, no grey traces?\n"
                 "Nothing changed or grey mess: try HOME (other panel type).");
}

void stepButtons() {
  Serial.println("4: button log. Press each button short, long and rapidly. Type a key to stop\n"
                 "(or press RST to start the test over).");
  Serial.println("t_ms       button  event     duration");
  bool down[3] = {};
  uint32_t since[3] = {};
  uint32_t changed[3] = {};
  while (!Serial.available()) {
    const uint32_t now = millis();
    for (int i = 0; i < 3; i++) {
      const bool d = pressed(kButtons[i]);
      if (d == down[i] || now - changed[i] < 25) continue;  // 25 ms debounce
      down[i] = d;
      changed[i] = now;
      if (d) {
        since[i] = now;
        Serial.printf("%-10u %-7s press\n", unsigned(now), kButtonNames[i]);
      } else {
        Serial.printf("%-10u %-7s release   %5u ms\n", unsigned(now), kButtonNames[i],
                      unsigned(now - since[i]));
      }
    }
    delay(1);
  }
  while (Serial.available()) Serial.read();
}

void runStep(int s) {
  step = s;
  Serial.printf("\n=== step %d, panel %s (%s) ===\n", s, kCandidates[res.candidate].name,
                kCandidates[res.candidate].note);
  if (s == 4) {
    stepButtons();
    return;
  }
  if (!panelAllowed()) return;
  switch (s) {
    case 1: stepFull(); break;
    case 2: stepFast(); break;
    case 3: stepPartial(); break;
  }
  printSummary();
}

// Next command: n, r, p, s, c, f, b or w.
char waitForCommand() {
  Serial.println("\n[NEXT = next step, PREV = repeat, HOME = other panel; or type n/r/p/s/c/f/b/w]");
  while (true) {
    if (Serial.available()) {
      const char ch = Serial.read();
      if (strchr("nrpscfbw", ch)) return ch;
    }
    for (int i = 0; i < 3; i++) {
      if (pressed(kButtons[i])) {
        delay(30);
        while (pressed(kButtons[i])) delay(10);
        return "rnp"[i];  // PREV, NEXT, HOME
      }
    }
    delay(10);
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  for (uint32_t t0 = millis(); !Serial && millis() - t0 < 4000;) delay(10);  // USB: wait for the monitor
  memset(&res, 0, sizeof(res));
  res.sdMb = res.sdPageMs = res.fullMs = res.fastMs = res.partialMs = -1;

  Serial.println("\nFireBeetle 2 ESP32-C6 + Waveshare 7.5\" V2: hardware test");
  Serial.printf("pins: panel SCK %d MOSI %d CS %d DC %d RST %d BUSY %d (reset %d ms, %u MHz) | "
                "card MISO %d CS %d | buttons PREV %d NEXT %d HOME %d | battery %d\n",
                kEpdSck, kEpdMosi, kEpdCs, kEpdDc, kEpdRst, kEpdBusy, kEpdResetMs,
                unsigned(kEpdSpiHz / 1000000), kSdMiso, kSdCs, kBtnPrev, kBtnNext, kBtnMenu,
                kBatAdc);
  checkBattery();
  checkButtons();
  checkLayout();
  checkSd();
  res.busyOk = probeUc81xx();
  Serial.printf("CONTROLLER-ID: %s\n", res.busyOk ? "UC81xx (BUSY high = ready)" : "not a UC81xx?");
  printSummary();
  if (res.busyOk) runStep(1);
}

void loop() {
  switch (waitForCommand()) {
    case 'n': runStep(step % 4 + 1); break;
    case 'r': runStep(step ? step : 1); break;
    case 'p':
      res.candidate = (res.candidate + 1) % kNumCandidates;
      runStep(1);
      break;
    case 's': printSummary(); break;
    case 'c':
      if (panelAllowed()) clearPanel();
      break;
    case 'f':
      driveAnyway = true;
      runStep(1);
      break;
    case 'b': busyTrace(); break;
    case 'w': blindRefresh(); break;
  }
}
