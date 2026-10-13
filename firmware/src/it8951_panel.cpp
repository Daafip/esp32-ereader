// IT8951 e-paper controller on SPI (M5Paper). The protocol follows M5GFX's
// Panel_IT8951 (tested on this board): before every 16-bit word wait for HRDY
// (busy pin high), and start each transfer with a preamble word: 0x6000 command,
// 0x0000 data, 0x1000 read (then one dummy word). The controller keeps the image
// in its own RAM, so partial updates (DU) need no "previous" buffer here.
#include "board.h"

#ifdef BOARD_PANEL_IT8951

#include <Arduino.h>
#include <SPI.h>
#include <esp_heap_caps.h>

#include "panel.h"

namespace panel {
namespace {

constexpr uint16_t kCmdSysRun = 0x0001;
constexpr uint16_t kCmdStandby = 0x0002;
constexpr uint16_t kCmdSleep = 0x0003;
constexpr uint16_t kCmdRegRead = 0x0010;
constexpr uint16_t kCmdRegWrite = 0x0011;
constexpr uint16_t kCmdLoadArea = 0x0021;
constexpr uint16_t kCmdLoadEnd = 0x0022;
constexpr uint16_t kCmdDisplayBufArea = 0x0037;
constexpr uint16_t kCmdDevInfo = 0x0302;
constexpr uint16_t kRegI80Cpcr = 0x0004;   // packed write
constexpr uint16_t kRegLisar = 0x0208;     // load image target address
constexpr uint16_t kRegLutAfsr = 0x1224;   // LUT engines busy (0 = idle)
// Waveform modes (M5Paper, M5GFX's epd_update_mode_t)
constexpr uint16_t kModeDu = 1;    // black/white, changed pixels only, no flash
constexpr uint16_t kModeGc16 = 2;  // full, flashing
constexpr uint16_t kModeGl16 = 3;  // full, no flash (text on white)
constexpr uint16_t kLoad4bppBigEndian = (1 << 8) | (2 << 4);  // endian, 4 bpp, no rotation

constexpr int kStride = kEpdNativeW / 8;
uint8_t *image = nullptr;          // native 1 bpp
uint32_t memAddr = 0x001236E0;     // M5GFX's fallback for the M5Paper
uint32_t writeHz = 2000000;        // slow until the controller has answered
uint32_t readHz = 2000000;
bool ready = false;
bool running = false;

bool waitReady(uint32_t timeoutMs = 3000) {
  const uint32_t start = millis();
  while (digitalRead(kEpdBusy) == LOW) {
    if (millis() - start > timeoutMs) return false;
    delayMicroseconds(20);
  }
  return true;
}

void select(uint32_t hz) {
  SPI.beginTransaction(SPISettings(hz, MSBFIRST, SPI_MODE0));
  digitalWrite(kEpdCs, LOW);
}

void deselect() {
  digitalWrite(kEpdCs, HIGH);
  SPI.endTransaction();
}

bool writeCommand(uint16_t cmd) {
  if (!waitReady()) return false;
  select(writeHz);
  SPI.transfer16(0x6000);
  SPI.transfer16(cmd);
  deselect();
  return true;
}

bool writeWord(uint16_t w) {
  if (!waitReady()) return false;
  select(writeHz);
  SPI.transfer16(0x0000);
  SPI.transfer16(w);
  deselect();
  return true;
}

// A command and its arguments, the arguments in one data transfer.
bool writeArgs(uint16_t cmd, const uint16_t *args, int n) {
  if (!writeCommand(cmd) || !waitReady()) return false;
  select(writeHz);
  SPI.transfer16(0x0000);
  bool ok = true;
  for (int i = 0; i < n && ok; i++) {
    ok = waitReady();
    if (ok) SPI.transfer16(args[i]);
  }
  deselect();
  return ok;
}

bool readWords(uint16_t *buf, int n) {
  if (!waitReady()) return false;
  select(readHz);
  SPI.transfer16(0x1000);
  SPI.transfer16(0);  // dummy
  for (int i = 0; i < n; i++) buf[i] = SPI.transfer16(0);
  deselect();
  return true;
}

bool writeReg(uint16_t addr, uint16_t value) {
  return writeCommand(kCmdRegWrite) && writeWord(addr) && writeWord(value);
}

uint16_t readReg(uint16_t addr) {
  uint16_t v = 0xFFFF;
  if (writeCommand(kCmdRegRead) && writeWord(addr)) readWords(&v, 1);
  return v;
}

// The previous display update has finished (all LUT engines idle).
void waitDisplayIdle(uint32_t timeoutMs = 5000) {
  const uint32_t start = millis();
  for (uint16_t v = readReg(kRegLutAfsr); v != 0 && v != 0xFFFF; v = readReg(kRegLutAfsr)) {
    if (millis() - start > timeoutMs) return;
    delay(2);
  }
}

void wake() {
  if (!running) running = writeCommand(kCmdSysRun);
}

}  // namespace

bool begin() {
  if (ready) return true;
  pinMode(kSdCs, OUTPUT);  // keep the card off the shared bus
  digitalWrite(kSdCs, HIGH);
  pinMode(kEpdCs, OUTPUT);
  digitalWrite(kEpdCs, HIGH);
  pinMode(kEpdBusy, INPUT_PULLUP);
  // Power on with a short low pulse first (M5GFX drives this pin as the reset).
  pinMode(kEpdPowerEn, OUTPUT);
  digitalWrite(kEpdPowerEn, LOW);
  delay(2);
  digitalWrite(kEpdPowerEn, HIGH);
  delay(10);
  SPI.begin(kSpiSck, kSpiMiso, kSpiMosi, -1);
  if (!waitReady(1000)) {
    Serial.println("IT8951: no answer (busy pin stays low)");
    return false;
  }
  uint16_t info[20] = {0};
  if (writeCommand(kCmdDevInfo)) {
    delay(1);
    readWords(info, 20);
  }
  const uint32_t addr = (uint32_t(info[3]) << 16) | info[2];
  if (addr != 0 && addr != 0xFFFFFFFFu) memAddr = addr;
  Serial.printf("IT8951: panel %ux%u, image buffer at 0x%08x\n", unsigned(info[0]),
                unsigned(info[1]), unsigned(memAddr));
  writeHz = kEpdSpiHz;
  readHz = kEpdSpiHz / 2;
  running = writeCommand(kCmdSysRun);
  writeReg(kRegI80Cpcr, 0x0001);  // packed write
  writeReg(kRegLisar + 2, uint16_t(memAddr >> 16));
  writeReg(kRegLisar, uint16_t(memAddr));
  image = static_cast<uint8_t *>(
      heap_caps_malloc(size_t(kStride) * kEpdNativeH, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!image) image = static_cast<uint8_t *>(malloc(size_t(kStride) * kEpdNativeH));
  ready = image != nullptr;
  return ready;
}

uint8_t *current() { return image; }
uint8_t *previous() { return nullptr; }  // the controller keeps it
void previousValid() {}

void update(Refresh mode) {
  if (!ready) return;
  wake();
  waitDisplayIdle();
  // Whole screen as 4 bpp (0x0 black, 0xF white), high nibble = left pixel:
  // the format M5GFX loads. 1-bpp loads would depend on the bit-order setting.
  const uint16_t area[5] = {kLoad4bppBigEndian, 0, 0, kEpdNativeW, kEpdNativeH};
  if (!writeArgs(kCmdLoadArea, area, 5) || !waitReady()) return;
  select(writeHz);
  SPI.transfer16(0x0000);
  uint8_t row[kEpdNativeW / 2];
  for (int y = 0; y < kEpdNativeH; y++) {
    const uint8_t *src = image + y * kStride;
    for (int i = 0; i < kStride; i++) {
      const uint8_t b = src[i];
      for (int k = 0; k < 4; k++) {
        const int shift = 7 - 2 * k;
        row[i * 4 + k] = uint8_t(((b >> shift) & 1 ? 0xF0 : 0x00) | ((b >> (shift - 1)) & 1 ? 0x0F : 0x00));
      }
    }
    SPI.writeBytes(row, sizeof(row));
  }
  deselect();
  writeCommand(kCmdLoadEnd);
  const uint16_t waveform = mode == Refresh::Full   ? kModeGc16
                            : mode == Refresh::Fast ? kModeGl16
                                                    : kModeDu;
  const uint16_t show[7] = {0, 0, kEpdNativeW, kEpdNativeH, waveform, uint16_t(memAddr),
                            uint16_t(memAddr >> 16)};
  writeArgs(kCmdDisplayBufArea, show, 7);
}

void standby() {
  if (!ready || !running) return;
  waitDisplayIdle();
  writeCommand(kCmdStandby);
  running = false;
}

void powerOff() {
  if (!ready) return;
  waitDisplayIdle();
  writeCommand(kCmdSleep);
  running = false;
  digitalWrite(kEpdPowerEn, LOW);
}

}  // namespace panel

#endif  // BOARD_PANEL_IT8951
