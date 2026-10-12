// Panel tester for the T5-2.13.
//
// Port of bb_epaper's examples/epd_panel_tester: that sketch drives its UI on
// an I2C OLED, the T5 has none, so this one talks over serial (115200) and
// uses the two buttons.
//
// Safety: the controller family (SSD16xx vs UC81xx) is probed from the BUSY
// line right after a hardware reset, *before* bb_epaper sends any init
// sequence. Candidates of the other family are never driven, because their
// power/voltage commands overlap and can misdrive the panel.
//
// Per candidate: full refresh of an identification screen, then 5 partial
// updates (if the panel supports them), then a fast refresh. Timings are
// printed as a RESULT line for docs/hardware.md.
//
// Controls (after each candidate):
//   USER (GPIO39) / 'n'  next candidate      BOOT (GPIO0) / 'r'  repeat
//   '0'..'9'             jump to candidate   'c'  clear panel to white
//   'l'                  list candidates     '?'  help
#include <Arduino.h>
#include <bb_epaper.h>

#include "board.h"

namespace {

struct Candidate {
  int panel;
  const char *name;
  const char *note;
};

// Every 2.13" panel bb_epaper knows that could plausibly sit on a T5.
const Candidate kCandidates[] = {
    {EP213_122x250, "EP213_122x250", "DEPG0213BN, T5 V2.3.1/V2.4 (SSD1680)"},
    {EP213B_122x250, "EP213B_122x250", "GDEY0213B74 (SSD1680)"},
    {EP213ZZ_122x250, "EP213ZZ_122x250", "LilyGo T3S3 panel, x_offset 1 (SSD1680)"},
    {EP213Z_122x250, "EP213Z_122x250", "CrowPanel 2.13 (SSD1680)"},
    {EP213Z_122x250_4GRAY, "EP213Z_122x250_4GRAY", "4-gray mode, GDEM0213B74? (SSD1680)"},
    {EP213_104x212, "EP213_104x212", "older 104x212 panel (UC8151)"},
};
constexpr int kNumCandidates = sizeof(kCandidates) / sizeof(kCandidates[0]);
constexpr int kPartialSteps = 5;

BBEPAPER epd;
int probedChip = BBEP_CHIP_NOT_DEFINED;

const char *chipName(int chip) {
  switch (chip) {
    case BBEP_CHIP_SSD16xx: return "SSD16xx";
    case BBEP_CHIP_UC81xx: return "UC81xx";
    default: return "unknown";
  }
}

void powerPanel() {
  // V2.4 gates panel power with GPIO12; on V2.3.1 the pin is unconnected.
  pinMode(kEpdPowerEn, OUTPUT);
  digitalWrite(kEpdPowerEn, HIGH);
  delay(100);
}

// Hardware reset; also wakes the controller from deep sleep.
void resetPanel() {
  pinMode(kEpdCs, OUTPUT);
  digitalWrite(kEpdCs, HIGH);
  pinMode(kEpdBusy, INPUT);
  pinMode(kEpdRst, OUTPUT);
  digitalWrite(kEpdRst, LOW);
  delay(40);
  digitalWrite(kEpdRst, HIGH);
}

// Same logic as bbepTestPanelType(), but without a preceding initIO(), which
// would already have sent the candidate's init sequence.
int probeChip() {
  resetPanel();
  Serial.print("BUSY after reset (ms:level):");
  int level = 0;
  for (int t = 0; t <= 200; t += 10) {
    level = digitalRead(kEpdBusy);
    if (t % 50 == 0) Serial.printf(" %d:%d", t, level);
    delay(10);
  }
  Serial.println();
  // SSD16xx: BUSY low = ready. UC81xx: BUSY high = ready.
  return level ? BBEP_CHIP_UC81xx : BBEP_CHIP_SSD16xx;
}

// Select a candidate and bring the panel up. Refuses a candidate whose
// controller family differs from the probed one.
bool beginPanel(const Candidate &c) {
  epd.freeBuffer();
  epd.setPanelType(c.panel);
  if (epd.getChip() != probedChip) {
    Serial.printf("SKIP: %s needs %s, panel answered as %s\n", c.name, chipName(epd.getChip()),
                  chipName(probedChip));
    return false;
  }
  resetPanel();
  delay(100);
  epd.initIO(kEpdDc, kEpdRst, kEpdBusy, kEpdCs, kEpdMosi, kEpdSck, kEpdSpiHz);
  epd.setRotation(kEpdRotation);
  return true;
}

void listCandidates() {
  for (int i = 0; i < kNumCandidates; i++) {
    BBEPAPER probe(kCandidates[i].panel);
    bool ok = probe.getChip() == probedChip;
    Serial.printf("  %d  %-22s %-8s %s%s\n", i, kCandidates[i].name, chipName(probe.getChip()),
                  kCandidates[i].note, ok ? "" : "  [skipped: wrong controller]");
  }
}

void help() {
  Serial.println("Controls: USER(39)/'n' next, BOOT(0)/'r' repeat, '0'-'9' jump, 'c' clear, "
                 "'l' list, '?' help");
}

// Identification screen; `counter` < 0 hides the partial-update counter box.
void drawScreen(const Candidate &c, int index, int counter) {
  const int w = epd.width();
  const int h = epd.height();
  epd.fillScreen(BBEP_WHITE);
  epd.drawRect(0, 0, w, h, BBEP_BLACK);
  // Corner markers: filled square top-left, hollow elsewhere -> shows mirroring.
  epd.fillRect(2, 2, 8, 8, BBEP_BLACK);
  epd.drawRect(w - 10, 2, 8, 8, BBEP_BLACK);
  epd.drawRect(2, h - 10, 8, 8, BBEP_BLACK);
  epd.drawRect(w - 10, h - 10, 8, 8, BBEP_BLACK);

  epd.setTextColor(BBEP_BLACK, BBEP_WHITE);
  epd.setFont(FONT_12x16);
  epd.setCursor(14, 4);
  epd.print(c.name);
  epd.setFont(FONT_8x8);
  epd.setCursor(14, 24);
  epd.printf("#%d  %s  %dx%d  rot %d", index, chipName(epd.getChip()), w, h, epd.getRotation());
  epd.setCursor(14, 36);
  epd.print("TOP EDGE ^  (top-left = filled)");
  epd.setCursor(14, 48);
  epd.print(epd.hasPartialRefresh() ? "partial: yes" : "partial: no");
  epd.print(epd.hasFastRefresh() ? "  fast: yes" : "  fast: no");

  if (epd.capabilities() & BBEP_4GRAY) {
    const int bw = (w - 28) / 4;
    for (int g = 0; g < 4; g++) {
      epd.fillRect(14 + g * bw, 62, bw, 20, BBEP_GRAY0 + g);
    }
    epd.drawRect(14, 62, bw * 4, 20, BBEP_BLACK);
  }
  if (counter >= 0) {
    epd.drawRect(w - 70, h - 40, 56, 30, BBEP_BLACK);
    epd.setFont(FONT_16x16);
    epd.setCursor(w - 58, h - 33);
    epd.printf("%02d", counter);
  }
}

void runCandidate(int index) {
  const Candidate &c = kCandidates[index];
  Serial.printf("\n=== #%d %s (%s) ===\n", index, c.name, c.note);
  if (!beginPanel(c)) return;
  if (epd.allocBuffer(false) != BBEP_SUCCESS) {
    Serial.println("ERROR: allocBuffer failed");
    return;
  }
  const bool gray = epd.capabilities() & BBEP_4GRAY;

  drawScreen(c, index, -1);
  // Same image into both controller RAMs, so a later partial update starts
  // from a known "old" image.
  epd.writePlane(gray ? PLANE_BOTH : PLANE_DUPLICATE);
  epd.refresh(REFRESH_FULL, true);
  const int fullMs = epd.opTime();
  Serial.printf("full refresh: data %d ms, refresh %d ms\n", epd.dataTime(), fullMs);

  int partialMs = -1;
  if (epd.hasPartialRefresh() && !gray) {
    int sum = 0;
    for (int i = 1; i <= kPartialSteps; i++) {
      drawScreen(c, index, i - 1);
      epd.writePlane(PLANE_0_TO_1);  // old image -> RAM 0x26
      drawScreen(c, index, i);
      epd.writePlane(PLANE_0);       // new image -> RAM 0x24
      epd.refresh(REFRESH_PARTIAL, true);
      Serial.printf("partial %d: data %d ms, refresh %d ms\n", i, epd.dataTime(), epd.opTime());
      sum += epd.opTime();
    }
    partialMs = sum / kPartialSteps;
  }

  int fastMs = -1;
  if (epd.hasFastRefresh() && !gray) {
    drawScreen(c, index, 99);
    epd.writePlane(PLANE_DUPLICATE);
    epd.refresh(REFRESH_FAST, true);
    fastMs = epd.opTime();
    Serial.printf("fast refresh: refresh %d ms\n", fastMs);
  }

  epd.sleep(DEEP_SLEEP);
  epd.freeBuffer();
  Serial.printf("RESULT panel=%s chip=%s size=%dx%d full_ms=%d partial_ms=%d fast_ms=%d\n", c.name,
                chipName(epd.getChip()), epd.width(), epd.height(), fullMs, partialMs, fastMs);
  Serial.println("Judge: text sharp? border complete? no noise/stripes? counter readable?");
}

void clearPanel(int index) {
  if (!beginPanel(kCandidates[index])) return;
  epd.fillScreen(BBEP_WHITE);  // bufferless: writes both RAMs directly
  epd.refresh(REFRESH_FULL, true);
  epd.sleep(DEEP_SLEEP);
  Serial.println("cleared");
}

// Returns the next command: 'n', 'r', 'c', 'l', '?', or a digit.
char waitForCommand() {
  Serial.println("\n[USER=next  BOOT=repeat  or type n/r/0-9/c/l/?]");
  while (true) {
    if (Serial.available()) {
      char ch = Serial.read();
      if (ch == 'n' || ch == 'r' || ch == 'c' || ch == 'l' || ch == '?' || isdigit(ch)) return ch;
    }
    for (int pin : {kBtnUser, kBtnBoot}) {
      if (digitalRead(pin) == LOW) {
        delay(30);
        while (digitalRead(pin) == LOW) delay(10);
        return pin == kBtnUser ? 'n' : 'r';
      }
    }
    delay(10);
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(kBtnUser, INPUT);  // GPIO39 has no internal pull-up
  pinMode(kBtnBoot, INPUT_PULLUP);
  Serial.println("\nT5-2.13 panel tester (bb_epaper 2.1.11)");
  Serial.printf("pins: MOSI %d SCK %d CS %d DC %d RST %d BUSY %d PWR %d, rotation %d\n", kEpdMosi,
                kEpdSck, kEpdCs, kEpdDc, kEpdRst, kEpdBusy, kEpdPowerEn, kEpdRotation);
  powerPanel();
  probedChip = probeChip();
  Serial.printf("CONTROLLER-ID: %s\n", chipName(probedChip));
  listCandidates();
  help();
}

void loop() {
  static int index = 0;
  runCandidate(index);
  while (true) {
    char cmd = waitForCommand();
    if (cmd == 'n') {
      index = (index + 1) % kNumCandidates;
      return;
    }
    if (cmd == 'r') return;
    if (isdigit(cmd) && cmd - '0' < kNumCandidates) {
      index = cmd - '0';
      return;
    }
    if (cmd == 'c') clearPanel(index);
    if (cmd == 'l') listCandidates();
    if (cmd == '?') help();
  }
}
