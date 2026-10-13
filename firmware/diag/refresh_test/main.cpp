// Refresh test for the T5-2.13 (Checkpoint 1, 2026-10-13).
//
// On David's panel a fast refresh only showed the new image right after a full
// one, and partial refreshes showed nothing. This sketch runs one numbered step
// per refresh variant; each step draws its number big plus a black block at its
// own position. Write down the numbers that appear on the glass: a step whose
// refresh doesn't work leaves the previous number standing.
//
// Round 2 (default): partial refresh with a custom waveform (LUT) instead of the
// panel's OTP one, four variants; each counts 1..5 with partial refreshes only,
// with a full refresh in between. A working variant changes the number without
// the screen flashing.
//
// At start: round 2. USER (GPIO39) or 's': round 2 again; '1': round 1.
#include <Arduino.h>
#include <bb_epaper.h>
#include <string.h>

#include <initializer_list>

#include "board.h"

namespace {

BBEPAPER epd;
uint8_t *buf = nullptr;
uint8_t prevImage[((122 + 7) / 8) * 250];  // the last image sent, for partial "old" RAM
constexpr uint32_t kPauseMs = 4000;        // time to read the number

void hardwareReset() {
  pinMode(kEpdCs, OUTPUT);
  digitalWrite(kEpdCs, HIGH);
  pinMode(kEpdBusy, INPUT);
  pinMode(kEpdRst, OUTPUT);
  digitalWrite(kEpdRst, LOW);
  delay(20);
  digitalWrite(kEpdRst, HIGH);
  delay(20);
}

uint32_t waitBusy(uint32_t limitMs = 20000) {
  const uint32_t t0 = millis();
  delay(10);
  while (epd.isBusy() && millis() - t0 < limitMs) delay(2);
  return millis() - t0;
}

void cmd(uint8_t c) { epd.writeCmd(c); }
void cmd(uint8_t c, std::initializer_list<uint8_t> data) {
  epd.writeCmd(c);
  uint8_t d[16];
  int n = 0;
  for (uint8_t b : data) d[n++] = b;
  epd.writeData(d, n);
}

// Update control 2 + activate, then wait: the raw "refresh".
uint32_t activate(uint8_t mode) {
  cmd(0x22, {mode});
  cmd(0x20);
  return waitBusy();
}

void draw(int step, const char *what) {
  epd.fillScreen(BBEP_WHITE);
  epd.drawRect(0, 0, epd.width(), epd.height(), BBEP_BLACK);
  epd.fillRect(8 + (step - 1) * 22, 70, 18, 40, BBEP_BLACK);  // moves with the step
  epd.setTextColor(BBEP_BLACK, BBEP_WHITE);
  epd.setFont(FONT_16x16);
  char n[8];
  snprintf(n, sizeof(n), "%d", step);
  epd.drawString(n, 10, 10);
  epd.setFont(FONT_8x8);
  epd.drawString(what, 50, 14);
}

void remember() { memcpy(prevImage, buf, sizeof(prevImage)); }

void report(int step, const char *what, uint32_t ms) {
  Serial.printf("step %2d  %-34s refresh %5u ms\n", step, what, unsigned(ms));
}

// bb_epaper's own sequence, as the reader uses it.
void bbRefresh(int step, const char *what, int mode, int plane) {
  draw(step, what);
  epd.writePlane(plane);
  const uint32_t t0 = millis();
  epd.refresh(mode, false);
  waitBusy();
  report(step, what, millis() - t0);
  remember();
  delay(kPauseMs);
}

// Partial by hand: old image -> 0x26, new -> 0x24, border, then `mode`.
void manualPartial(int step, const char *what, uint8_t mode) {
  memcpy(buf, prevImage, sizeof(prevImage));
  epd.writePlane(PLANE_0_TO_1);
  draw(step, what);
  epd.writePlane(PLANE_0);
  cmd(0x3C, {0x80});
  report(step, what, activate(mode));
  remember();
  delay(kPauseMs);
}

void runSequence() {
  Serial.println("\n--- refresh test: note which numbers appear ---");
  bbRefresh(1, "FULL", REFRESH_FULL, PLANE_DUPLICATE);
  bbRefresh(2, "FAST after full", REFRESH_FAST, PLANE_DUPLICATE);
  bbRefresh(3, "FAST after fast", REFRESH_FAST, PLANE_DUPLICATE);

  // 4: hardware reset before writing (the reader's current attempt)
  hardwareReset();
  bbRefresh(4, "RESET + FAST", REFRESH_FAST, PLANE_DUPLICATE);

  // 5: clock + analog on (0x22 0xC0) before writing, as Waveshare does
  activate(0xC0);
  bbRefresh(5, "POWER ON + FAST", REFRESH_FAST, PLANE_DUPLICATE);

  // 6: fast init first, image written after it, then display (manual)
  {
    const char *what = "FAST INIT, THEN IMAGE";
    const uint32_t t0 = millis();
    cmd(0x12);
    waitBusy();
    cmd(0x01, {0xF9, 0x00, 0x00});
    cmd(0x11, {0x03});
    cmd(0x3C, {0x01});
    cmd(0x18, {0x80});
    activate(0xB1);
    cmd(0x1A, {0x64, 0x00});
    activate(0x91);
    draw(6, what);
    epd.writePlane(PLANE_DUPLICATE);
    activate(0xC7);
    report(6, what, millis() - t0);
    remember();
    delay(kPauseMs);
  }

  // 7: the same with the old image in 0x26 instead of a duplicate
  {
    const char *what = "FAST INIT, OLD+NEW";
    const uint32_t t0 = millis();
    cmd(0x12);
    waitBusy();
    cmd(0x01, {0xF9, 0x00, 0x00});
    cmd(0x11, {0x03});
    cmd(0x3C, {0x01});
    cmd(0x18, {0x80});
    activate(0xB1);
    cmd(0x1A, {0x64, 0x00});
    activate(0x91);
    memcpy(buf, prevImage, sizeof(prevImage));
    epd.writePlane(PLANE_0_TO_1);
    draw(7, what);
    epd.writePlane(PLANE_0);
    activate(0xC7);
    report(7, what, millis() - t0);
    remember();
    delay(kPauseMs);
  }

  bbRefresh(8, "FULL", REFRESH_FULL, PLANE_DUPLICATE);
  bbRefresh(9, "FAST after full (again)", REFRESH_FAST, PLANE_DUPLICATE);

  // 10-13: partial variants (OTP display mode 2) with old/new RAM
  manualPartial(10, "PARTIAL 0xFF", 0xFF);
  manualPartial(11, "PARTIAL 0xFC", 0xFC);
  manualPartial(12, "PARTIAL 0xCF", 0xCF);
  manualPartial(13, "PARTIAL 0x0C", 0x0C);

  bbRefresh(14, "FULL (end)", REFRESH_FULL, PLANE_DUPLICATE);
  Serial.println("--- done: which numbers did you see? USER or 's' runs it again ---");
}

// --- round 2: custom partial LUT ------------------------------------------------

// SSD1680 partial waveform: bb_epaper's epd296_init_sequence_part (Waveshare 2.9"
// V2), 5 x 12 voltage selections, 12 x 7 phase timings, frame rate + XON.
const uint8_t kPartialLut[153] = {
    0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  //
    0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  //
    0x40, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  //
    0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,                                //
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                //
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                //
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                //
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                //
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                //
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                //
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                //
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                //
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                //
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                //
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                //
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00,
};
constexpr int kTp0 = 60;  // first phase: TP[0A]
constexpr int kRp0 = 66;  // its repeat count

struct Variant {
  char letter;
  const char *name;
  uint8_t tp0, rp0, vsh2, update;
};
const Variant kVariants[] = {
    {'A', "LUT 2.9in, 0x0F", 0x0A, 0x02, 0xB0, 0x0F},
    {'B', "LUT 2.13in timing, 0x0F", 0x14, 0x00, 0x00, 0x0F},
    {'C', "LUT 2.9in, 0xCF", 0x0A, 0x02, 0xB0, 0xCF},
    {'D', "LUT stronger, 0x0F", 0x14, 0x02, 0xB0, 0x0F},
};

void drawLabel(char letter, int n, const char *what) {
  epd.fillScreen(BBEP_WHITE);
  epd.drawRect(0, 0, epd.width(), epd.height(), BBEP_BLACK);
  epd.fillRect(8 + (n - 1) * 40, 70, 30, 40, BBEP_BLACK);
  epd.setTextColor(BBEP_BLACK, BBEP_WHITE);
  epd.setFont(FONT_16x16);
  char t[8];
  snprintf(t, sizeof(t), "%c%d", letter, n);
  epd.drawString(t, 10, 10);
  epd.setFont(FONT_8x8);
  epd.drawString(what, 60, 14);
}

void loadPartialLut(const Variant &v) {
  hardwareReset();  // as Waveshare does before each partial update (RAM stays)
  waitBusy(1000);
  cmd(0x01, {0xF9, 0x00, 0x00});
  cmd(0x11, {0x03});
  uint8_t lut[sizeof(kPartialLut)];
  memcpy(lut, kPartialLut, sizeof(lut));
  lut[kTp0] = v.tp0;
  lut[kRp0] = v.rp0;
  epd.writeCmd(0x32);
  epd.writeData(lut, sizeof(lut));
  cmd(0x3F, {0x22});
  cmd(0x03, {0x17});
  cmd(0x04, {0x41, v.vsh2, 0x32});
  cmd(0x2C, {0x36});
  cmd(0x37, {0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00});
  cmd(0x3C, {0x80});
  activate(0xC0);  // clock + analog on
}

void partialStep(const Variant &v, int n) {
  loadPartialLut(v);
  memcpy(buf, prevImage, sizeof(prevImage));
  epd.writePlane(PLANE_0_TO_1);  // old image -> 0x26
  drawLabel(v.letter, n, v.name);
  epd.writePlane(PLANE_0);       // new image -> 0x24
  const uint32_t ms = activate(v.update);
  Serial.printf("%c%d  %-26s refresh %5u ms\n", v.letter, n, v.name, unsigned(ms));
  remember();
  delay(2000);
}

void fullReset(int n) {
  hardwareReset();
  waitBusy(1000);
  epd.initIO(kEpdDc, kEpdRst, kEpdBusy, kEpdCs, kEpdMosi, kEpdSck, kEpdSpiHz);  // init again
  epd._bbep.x_offset = kEpdXOffsetBytes;
  epd.setRotation(kEpdRotation);
  epd.setBuffer(buf);
  char what[24];
  snprintf(what, sizeof(what), "FULL %d", n);
  bbRefresh(n, what, REFRESH_FULL, PLANE_DUPLICATE);
}

void runLutTest() {
  Serial.println("\n--- round 2: custom partial LUT. Which letters counted 1..5 without flashing? ---");
  int n = 1;
  for (const Variant &v : kVariants) {
    fullReset(n++);
    for (int i = 1; i <= 5; i++) partialStep(v, i);
  }
  fullReset(n);
  Serial.println("--- done. USER or 's': again ---");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(kBtnUser, INPUT);
  pinMode(kEpdPowerEn, OUTPUT);
  digitalWrite(kEpdPowerEn, HIGH);
  delay(100);
  epd.setPanelType(kEpdPanel);
  hardwareReset();
  epd.initIO(kEpdDc, kEpdRst, kEpdBusy, kEpdCs, kEpdMosi, kEpdSck, kEpdSpiHz);
  epd._bbep.x_offset = kEpdXOffsetBytes;
  epd.setRotation(kEpdRotation);
  epd.allocBuffer(false);
  buf = static_cast<uint8_t *>(epd.getBuffer());
  memset(prevImage, 0xFF, sizeof(prevImage));
  Serial.println("\nT5-2.13 refresh test");
  runLutTest();
}

void loop() {
  const int c = Serial.available() ? Serial.read() : -1;
  if (c == '1') runSequence();
  if (c == 's' || digitalRead(kBtnUser) == LOW) {
    while (digitalRead(kBtnUser) == LOW) delay(10);
    runLutTest();
  }
  delay(10);
}
