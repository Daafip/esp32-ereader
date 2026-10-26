// SPI e-paper with its own controller RAM, through bb_epaper: the T5-2.13 (SSD1680)
// and the 7.5" Waveshare panel (UC81xx). A copy of the shown image serves partial
// refreshes after deep sleep: in RTC memory where a page fits (T5-2.13), otherwise
// in RAM (BOARD_SHOWN_IN_RAM), drawn again by the app after deep sleep.
#include "board.h"

#ifdef BOARD_PANEL_BBEPAPER

#include <Arduino.h>
#include <SPI.h>
#include <driver/gpio.h>
#include <esp_attr.h>
#include <soc/soc_caps.h>
#include <string.h>

#include "display.h"

namespace display {
namespace {

BBEPAPER panel(kEpdPanel);
// bb_epaper sizes its buffer by the native panel (T5-2.13: 16 x 250 = 4000 B); a
// page uses the first kPageBytes of it.
uint8_t fb[((kEpdNativeW + 7) / 8) * kEpdNativeH];
static_assert(sizeof(fb) >= kPageBytes, "framebuffer too small for a page");

// The image on the glass. RTC memory survives deep sleep and is zeroed on
// power-on; a 7.5" page (48 KB) is bigger than the C6's 16 KB of it.
#ifdef BOARD_SHOWN_IN_RAM
#define SHOWN_ATTR
#else
#define SHOWN_ATTR RTC_DATA_ATTR
#endif
SHOWN_ATTR uint8_t shownImage[kPageBytes];
SHOWN_ATTR bool shownImageValid = false;
RTC_DATA_ATTR bool rtcKeptPowered = false;
RTC_DATA_ATTR uint16_t rtcPartials = 0;
RTC_DATA_ATTR int16_t rtcRotation = -1;  // rotation of the shown image; -1 = power-on

bool panelRamValid = false;  // controller's "old" RAM equals shownImage
Timing timing;

const int kHeldPins[] = {kEpdPowerEn, kEpdRst, kEpdCs};

void releaseHolds() {
#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
  gpio_deep_sleep_hold_dis();
#endif
  for (int pin : kHeldPins) {
    if (pin >= 0) gpio_hold_dis(gpio_num_t(pin));
  }
}

// Keeps pin levels through ESP32 deep sleep. With the panel powered (always, on
// boards without a power pin), RST and CS stay high (no reset, no stray commands).
// With power cut, only the power pin is held (low): driving RST/CS high would
// back-power the panel. The C6 holds single pins in deep sleep by itself.
void holdPins(bool panelPowered) {
  for (int pin : kHeldPins) {
    if (pin >= 0 && (panelPowered || kEpdPowerEn < 0 || pin == kEpdPowerEn)) {
      gpio_hold_en(gpio_num_t(pin));
    }
  }
#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
  gpio_deep_sleep_hold_en();
#endif
}

// The card on the panel's bus (kSdSharedSpi) leaves the bus at its own clock, and
// bb_epaper writes without transactions: set the panel's clock first.
void claimBus() {
  if (!kSdSharedSpi) return;
  SPI.beginTransaction(SPISettings(kEpdSpiHz, MSBFIRST, SPI_MODE0));
  SPI.endTransaction();
}

void waitIdle(uint32_t limitMs) {
  const uint32_t t0 = millis();
  delay(10);
  while (panel.isBusy() && millis() - t0 < limitMs) delay(2);
}

void command(uint8_t cmd, const uint8_t *data, int len) {
  panel.writeCmd(cmd);
  if (len) panel.writeData(const_cast<uint8_t *>(data), len);
}

// SSD1680 fast refresh, prepared before the image is written. bb_epaper sends the
// same preparation (its EP213Z fast init: soft resets, then the LUT loaded for a
// forced 100 C) only after the image, and on this panel the image then only
// showed when the previous refresh was a full one (refresh_test, 2026-10-13:
// steps 3-5 failed, 6-7 with this order worked). The caller then writes the
// image and sends 0x22 0xC7 / 0x20.
void sendFastInit() {
  static const uint8_t driver[] = {0xF9, 0x00, 0x00};  // 250 gate lines
  static const uint8_t entry[] = {0x03};               // x then y increment
  static const uint8_t border[] = {0x01};
  static const uint8_t sensor[] = {0x80};              // internal temperature sensor
  static const uint8_t temp100[] = {0x64, 0x00};       // 100 C: the short waveform
  command(0x12, nullptr, 0);  // soft reset
  waitIdle(1000);
  command(0x01, driver, sizeof(driver));
  command(0x11, entry, sizeof(entry));
  command(0x3C, border, sizeof(border));
  command(0x18, sensor, sizeof(sensor));
  panel.writeCmd2(0x22, 0xB1);  // load temperature + LUT
  panel.writeCmd(0x20);
  waitIdle(1000);
  command(0x1A, temp100, sizeof(temp100));
  panel.writeCmd2(0x22, 0x91);  // load the LUT for that temperature
  panel.writeCmd(0x20);
  waitIdle(1000);
}

// SSD1680 partial waveform: bb_epaper's epd296_init_sequence_part (Waveshare 2.9"
// V2) with the first phase as in Waveshare's 2.13" V3 driver (20 frames, no
// repeat): 5 x 12 voltage selections, 12 x 7 phase timings, frame rate + XON.
// refresh_test round 2 (2026-10-13): 632 ms, no flash.
const uint8_t kPartialLut[153] = {
    0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  //
    0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  //
    0x40, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  //
    0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                //
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

void hardwareReset();

// Controller ready for a partial update with kPartialLut (Waveshare's sequence:
// reset, LUT, voltages, display option, border, clock + analog on). The reset
// keeps the RAM. The caller writes old (0x26) and new (0x24) and sends 0x22 0x0F.
void loadPartialLut() {
  static const uint8_t driver[] = {0xF9, 0x00, 0x00};
  static const uint8_t entry[] = {0x03};
  static const uint8_t eopt[] = {0x22};
  static const uint8_t gate[] = {0x17};                // VGH
  static const uint8_t source[] = {0x41, 0x00, 0x32};  // VSH1, VSH2, VSL
  static const uint8_t vcom[] = {0x36};
  static const uint8_t option[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00};
  static const uint8_t border[] = {0x80};
  hardwareReset();
  waitIdle(1000);
  command(0x01, driver, sizeof(driver));
  command(0x11, entry, sizeof(entry));
  command(0x32, kPartialLut, sizeof(kPartialLut));
  command(0x3F, eopt, sizeof(eopt));
  command(0x03, gate, sizeof(gate));
  command(0x04, source, sizeof(source));
  command(0x2C, vcom, sizeof(vcom));
  command(0x37, option, sizeof(option));
  command(0x3C, border, sizeof(border));
  panel.writeCmd2(0x22, 0xC0);  // clock + analog on
  panel.writeCmd(0x20);
  waitIdle(1000);
}

void hardwareReset() {
  pinMode(kEpdCs, OUTPUT);
  digitalWrite(kEpdCs, HIGH);
  pinMode(kEpdBusy, INPUT);
  pinMode(kEpdRst, OUTPUT);
  digitalWrite(kEpdRst, LOW);
  delay(kEpdResetMs);
  digitalWrite(kEpdRst, HIGH);
  delay(20);
}

}  // namespace

void begin() {
  releaseHolds();
  if (kEpdPowerEn >= 0) {
    pinMode(kEpdPowerEn, OUTPUT);
    digitalWrite(kEpdPowerEn, HIGH);
    if (!rtcKeptPowered) delay(10);  // let the panel supply settle
  }
  if (kSdSharedSpi) {
    // One bus with the card: keep the card deselected, and start the bus with its
    // MISO (bb_epaper starts it without one, and leaves a running bus as it is).
    pinMode(kSdCs, OUTPUT);
    digitalWrite(kSdCs, HIGH);
    SPI.begin(kSdSck, kSdMiso, kSdMosi, -1);
  }
  hardwareReset();  // also leaves the controller's deep sleep
  panel.initIO(kEpdDc, kEpdRst, kEpdBusy, kEpdCs, kEpdMosi, kEpdSck, kEpdSpiHz);
  panel._bbep.x_offset = kEpdXOffsetBytes;  // used by every RAM window bb_epaper sets
  if (rtcRotation < 0) rtcRotation = kEpdRotation;
  panel.setRotation(rtcRotation);
  panel.setBuffer(fb);
  panelRamValid = rtcKeptPowered && shownImageValid;
  rtcKeptPowered = false;
}

void sleep(bool keepPower) {
  claimBus();
  panel.sleep(keepPower ? LIGHT_SLEEP : DEEP_SLEEP);  // SSD16xx: 0x10 = 0x01 keeps RAM, 0x03 not
  rtcKeptPowered = keepPower;
  if (!keepPower && kEpdPowerEn >= 0) digitalWrite(kEpdPowerEn, LOW);
  holdPins(keepPower);
}

BBEPAPER &epd() { return panel; }
uint8_t *framebuffer() { return fb; }
void clear() { memset(fb, 0xFF, sizeof(fb)); }

Refresh show(Refresh mode) {
  if (mode == Refresh::Partial && !kEpdPartialWorks) mode = Refresh::Fast;
  if (mode == Refresh::Partial && !shownImageValid) mode = Refresh::Full;
  if (mode == Refresh::Fast && !panel.hasFastRefresh()) mode = Refresh::Full;

  claimBus();
  const uint32_t t0 = millis();
  if (mode == Refresh::Fast && kEpdFastInitFirst) {
    sendFastInit();  // before the image, see there
    panel.writePlane(PLANE_DUPLICATE);
  } else if (mode == Refresh::Partial && kEpdCustomPartialLut) {
    loadPartialLut();
    panel.setBuffer(shownImage);  // what the glass shows -> "old" RAM
    panel.writePlane(PLANE_0_TO_1);
    panel.setBuffer(fb);
    panel.writePlane(PLANE_0);
  } else if (mode == Refresh::Partial) {
    if (!panelRamValid) {  // tell the controller what is on the glass now
      panel.setBuffer(shownImage);
      panel.writePlane(PLANE_0_TO_1);
      panel.setBuffer(fb);
    }
    panel.writePlane(PLANE_0);
  } else {
    panel.writePlane(PLANE_DUPLICATE);
  }
  const uint32_t t1 = millis();
  if (mode == Refresh::Fast && kEpdFastInitFirst) {
    panel.writeCmd2(0x22, 0xC7);  // display mode 1 with the loaded LUT, then power down
    panel.writeCmd(0x20);
  } else if (mode == Refresh::Partial && kEpdCustomPartialLut) {
    panel.writeCmd2(0x22, 0x0F);  // display mode 2 with our LUT, then power down
    panel.writeCmd(0x20);
  } else {
    panel.refresh(mode == Refresh::Full   ? REFRESH_FULL
                  : mode == Refresh::Fast ? REFRESH_FAST
                                          : REFRESH_PARTIAL,
                  false);
  }
  // Our own wait: bb_epaper gives up after 5 s, and commands sent while the
  // controller is busy get lost.
  delay(10);
  while (panel.isBusy() && millis() - t1 < kEpdBusyTimeoutMs) delay(5);
  const uint32_t t2 = millis();
  if (mode == Refresh::Partial && !kEpdCustomPartialLut) {
    // SSD16xx may not copy new -> old RAM after a partial update; do it, so
    // the next partial update can skip resending the old image.
    panel.writePlane(PLANE_0_TO_1);
  }
  timing = {t1 - t0, t2 - t1};
  Serial.printf("display: %s refresh, data %u ms, refresh %u ms\n",
                mode == Refresh::Full ? "full" : mode == Refresh::Fast ? "fast" : "partial",
                unsigned(t1 - t0), unsigned(t2 - t1));

  memcpy(shownImage, fb, kPageBytes);
  shownImageValid = true;
  panelRamValid = true;
  rtcPartials = mode == Refresh::Partial ? rtcPartials + 1 : 0;
  return mode;
}

void setRotation(int degrees) { panel.setRotation(degrees); }

void setOrientation(uint8_t orientation) {
  const int16_t r = int16_t(rotationFor(orientation));
  if (r == rtcRotation) return;
  rtcRotation = r;
  panel.setRotation(r);
  shownImageValid = false;  // the glass shows the other orientation: next one is full
  panelRamValid = false;
}

int screenIndex() { return rtcRotation < 0 ? 0 : screenForRotation(rtcRotation); }
const ScreenSpec &screen() { return kScreens[screenIndex()]; }

void drawText6x8(const char *ascii, int x, int y, int color, int bg) {
  panel.setFont(FONT_6x8);
  panel.setTextColor(color, bg);
  char one[2] = {0, 0};
  for (; *ascii && x + 6 <= panel.width(); ascii++, x += 6) {
    one[0] = (*ascii >= 32 && *ascii < 127) ? *ascii : '?';
    panel.drawString(one, x, y);
  }
}

void assumeShown() {
  memcpy(shownImage, fb, kPageBytes);
  shownImageValid = true;
  panelRamValid = false;
}

bool shownValid() { return shownImageValid; }
uint16_t partialsSinceFull() { return rtcPartials; }
Timing lastTiming() { return timing; }

}  // namespace display

#endif  // BOARD_PANEL_BBEPAPER
