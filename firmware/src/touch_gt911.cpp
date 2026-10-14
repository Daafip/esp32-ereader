// GT911 touch controller on I2C. Register map from the GT911 programming guide
// (and M5GFX's Touch_GT911): 0x814E = status (bit 7: new report, bits 0-3:
// number of points), 0x814F = first point (id, x lo/hi, y lo/hi, size lo/hi).
// The M5 boards configure INT as a level: it stays low until the status is cleared.
#include "touch.h"

#include "board.h"

#ifdef BOARD_HAS_TOUCH

#include <Arduino.h>
#include <Wire.h>

namespace touch {
namespace {

constexpr uint16_t kRegStatus = 0x814E;
constexpr uint16_t kRegPoint1 = 0x814F;
uint8_t address = 0;
bool fingerDown = false;
bool flipped = false;  // Settings -> Flip screen
int lastX = 0, lastY = 0;

bool readRegs(uint16_t reg, uint8_t *buf, size_t len) {
  Wire.beginTransmission(address);
  Wire.write(uint8_t(reg >> 8));
  Wire.write(uint8_t(reg));
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(address, uint8_t(len)) != len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = uint8_t(Wire.read());
  return true;
}

void clearStatus() {
  Wire.beginTransmission(address);
  Wire.write(uint8_t(kRegStatus >> 8));
  Wire.write(uint8_t(kRegStatus));
  Wire.write(uint8_t(0));
  Wire.endTransmission();
}

}  // namespace

bool begin() {
  pinMode(kTouchInt, INPUT);  // M5Paper GPIO36 is input-only; the boards pull INT up
  Wire.begin(kTouchSda, kTouchScl, 400000);
  for (uint8_t a : {uint8_t(0x5D), uint8_t(0x14)}) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      address = a;
      clearStatus();
      return true;
    }
  }
  Serial.println("touch: no GT911 found");
  return false;
}

bool pending() { return address && digitalRead(kTouchInt) == LOW; }
int interruptPin() { return address ? kTouchInt : -1; }
void setFlipped(bool on) { flipped = on; }

bool read(int &x, int &y) {
  if (!address) return false;
  if (pending() || fingerDown) {
    uint8_t status = 0;
    if (readRegs(kRegStatus, &status, 1) && (status & 0x80)) {
      const int points = status & 0x0F;
      uint8_t p[8];
      if (points > 0 && points <= 5 && readRegs(kRegPoint1, p, sizeof(p))) {
        int rx = p[1] | (p[2] << 8);
        int ry = p[3] | (p[4] << 8);
        if (kTouchFlip != flipped) {
          rx = kTouchRawW - 1 - rx;
          ry = kTouchRawH - 1 - ry;
        }
        // raw portrait range onto the page (same size on the M5 boards)
        lastX = rx * kPageW / kTouchRawW;
        lastY = ry * kPageH / kTouchRawH;
        fingerDown = true;
      } else {
        fingerDown = false;
      }
      clearStatus();
    }
  }
  x = lastX;
  y = lastY;
  return fingerDown;
}

}  // namespace touch

#else  // no touch screen

namespace touch {
bool begin() { return false; }
bool read(int &, int &) { return false; }
bool pending() { return false; }
int interruptPin() { return -1; }
void setFlipped(bool) {}
}  // namespace touch

#endif
