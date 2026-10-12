// T5-2.13 reader firmware. Placeholder until Phase 1/3: shows a splash screen
// with the provisional panel constant so `pixi run fw-build` exercises board.h.
#include <Arduino.h>
#include <bb_epaper.h>

#include "board.h"

BBEPAPER epd(kEpdPanel);

void setup() {
  Serial.begin(115200);
  pinMode(kEpdPowerEn, OUTPUT);
  digitalWrite(kEpdPowerEn, HIGH);
  delay(100);
  epd.initIO(kEpdDc, kEpdRst, kEpdBusy, kEpdCs, kEpdMosi, kEpdSck, kEpdSpiHz);
  epd.setRotation(kEpdRotation);
  epd.allocBuffer(false);
  epd.fillScreen(BBEP_WHITE);
  epd.setFont(FONT_12x16);
  epd.setTextColor(BBEP_BLACK, BBEP_WHITE);
  epd.setCursor(8, 8);
  epd.print("T5 Reader");
  epd.setFont(FONT_8x8);
  epd.setCursor(8, 32);
  epd.print("Phase 0 - run the diag sketches");
  epd.writePlane(PLANE_DUPLICATE);
  epd.refresh(REFRESH_FULL, true);
  epd.sleep(DEEP_SLEEP);
  Serial.println("splash shown");
}

void loop() { delay(1000); }
