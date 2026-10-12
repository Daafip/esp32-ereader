// TF card test for the T5-2.13.
//
// 1. Bus check: are the SD lines (14/15) wired to the EPD lines (18/23)?
//    Toggles SD_SCK/SD_MOSI as GPIO and reads EPD_SCK/EPD_MOSI back.
// 2. Mount the card on HSPI (SCK 14, MISO 2, MOSI 15, CS 13), print card info,
//    list the root.
// 3. Write/read/verify a 4 KB file.
// 4. Write a 400-page dummy book (400 x 3904 B = 1.5 MB) and time 50 random
//    page reads -- the exact access pattern of the reader.
// 5. Delete the test files.
//
// Reaching this sketch's output at all with a card inserted proves the board
// boots with the card in (GPIO2/GPIO15 are strapping pins).
#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <esp_system.h>

#include "board.h"

namespace {

constexpr size_t kPageBytes = 32 * 122;  // one landscape 250x122 page, 1 bpp
constexpr int kBookPages = 400;
constexpr int kRandomReads = 50;
const char *kSmallFile = "/t5test_4k.bin";
const char *kBookFile = "/t5test_book.bin";

SPIClass sdSpi(HSPI);
uint8_t buf[4096];

const char *resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return "other";
  }
}

// Returns true if `in` follows `out` for both levels.
bool wired(int out, int in) {
  pinMode(out, OUTPUT);
  pinMode(in, INPUT_PULLDOWN);
  digitalWrite(out, HIGH);
  delayMicroseconds(50);
  const int hi = digitalRead(in);
  digitalWrite(out, LOW);
  delayMicroseconds(50);
  const int lo = digitalRead(in);
  pinMode(out, INPUT);
  pinMode(in, INPUT);
  return hi == HIGH && lo == LOW;
}

void busCheck() {
  // Keep both chip selects inactive so neither device sees the toggling.
  pinMode(kEpdCs, OUTPUT);
  digitalWrite(kEpdCs, HIGH);
  pinMode(kSdCs, OUTPUT);
  digitalWrite(kSdCs, HIGH);
  const bool sck = wired(kSdSck, kEpdSck);
  const bool mosi = wired(kSdMosi, kEpdMosi);
  Serial.printf("BUS: SD_SCK(%d)->EPD_SCK(%d) %s, SD_MOSI(%d)->EPD_MOSI(%d) %s => %s\n", kSdSck,
                kEpdSck, sck ? "connected" : "separate", kSdMosi, kEpdMosi,
                mosi ? "connected" : "separate", (sck || mosi) ? "SHARED bus" : "SEPARATE buses");
}

bool mount() {
  sdSpi.begin(kSdSck, kSdMiso, kSdMosi, kSdCs);
  const uint32_t speeds[] = {kSdSpiHz, 10000000, 4000000};
  for (uint32_t hz : speeds) {
    if (SD.begin(kSdCs, sdSpi, hz)) {
      Serial.printf("MOUNT: ok at %u Hz\n", (unsigned)hz);
      return true;
    }
    Serial.printf("MOUNT: failed at %u Hz\n", (unsigned)hz);
    SD.end();
  }
  return false;
}

void cardInfo() {
  const char *type = "unknown";
  switch (SD.cardType()) {
    case CARD_MMC: type = "MMC"; break;
    case CARD_SD: type = "SDSC"; break;
    case CARD_SDHC: type = "SDHC/SDXC"; break;
    default: break;
  }
  Serial.printf("CARD: %s, %llu MB, fs total %llu MB, used %llu MB\n", type,
                SD.cardSize() >> 20, SD.totalBytes() >> 20, SD.usedBytes() >> 20);
}

void listRoot() {
  File root = SD.open("/");
  if (!root) {
    Serial.println("LIST: cannot open /");
    return;
  }
  Serial.println("LIST /:");
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    Serial.printf("  %s%s  %u\n", f.name(), f.isDirectory() ? "/" : "", (unsigned)f.size());
  }
}

uint8_t pattern(size_t i) { return (uint8_t)(i * 31 + 7); }

bool smallFileTest() {
  for (size_t i = 0; i < sizeof(buf); i++) buf[i] = pattern(i);
  uint32_t t0 = micros();
  File f = SD.open(kSmallFile, FILE_WRITE);
  const bool written = f && f.write(buf, sizeof(buf)) == sizeof(buf);
  f.close();
  const uint32_t writeUs = micros() - t0;
  if (!written) {
    Serial.println("4K: write FAILED");
    return false;
  }
  memset(buf, 0, sizeof(buf));
  t0 = micros();
  f = SD.open(kSmallFile, FILE_READ);
  bool ok = f && f.read(buf, sizeof(buf)) == sizeof(buf);
  f.close();
  const uint32_t readUs = micros() - t0;
  for (size_t i = 0; ok && i < sizeof(buf); i++) ok = buf[i] == pattern(i);
  Serial.printf("4K: write %u us, read %u us, verify %s\n", (unsigned)writeUs, (unsigned)readUs,
                ok ? "OK" : "FAILED");
  return ok;
}

bool randomReadTest() {
  File f = SD.open(kBookFile, FILE_WRITE);
  if (!f) {
    Serial.println("BOOK: create FAILED");
    return false;
  }
  uint32_t t0 = millis();
  for (int p = 0; p < kBookPages; p++) {
    memset(buf, (uint8_t)p, kPageBytes);
    if (f.write(buf, kPageBytes) != kPageBytes) {
      Serial.printf("BOOK: write FAILED at page %d\n", p);
      f.close();
      return false;
    }
  }
  f.close();
  const uint32_t writeMs = millis() - t0;
  Serial.printf("BOOK: wrote %d pages (%u KB) in %u ms\n", kBookPages,
                (unsigned)(kBookPages * kPageBytes / 1024), (unsigned)writeMs);

  f = SD.open(kBookFile, FILE_READ);
  uint32_t minUs = UINT32_MAX, maxUs = 0, sumUs = 0;
  bool ok = (bool)f;
  for (int i = 0; ok && i < kRandomReads; i++) {
    const int page = (int)(esp_random() % kBookPages);
    t0 = micros();
    ok = f.seek((uint32_t)page * kPageBytes) && f.read(buf, kPageBytes) == kPageBytes;
    const uint32_t us = micros() - t0;
    ok = ok && buf[0] == (uint8_t)page && buf[kPageBytes - 1] == (uint8_t)page;
    minUs = min(minUs, us);
    maxUs = max(maxUs, us);
    sumUs += us;
  }
  f.close();
  Serial.printf("RANDOM READ %u B x %d: min %u us, avg %u us, max %u us, data %s\n",
                (unsigned)kPageBytes, kRandomReads, (unsigned)minUs,
                (unsigned)(sumUs / kRandomReads), (unsigned)maxUs, ok ? "OK" : "FAILED");
  return ok;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nT5-2.13 TF card test");
  Serial.printf("BOOT: reset reason %s. If a card is inserted, booting with it works.\n",
                resetReason());
  busCheck();
  if (!mount()) {
    Serial.println("RESULT: mount FAILED (card inserted? FAT32? pins?)");
    return;
  }
  cardInfo();
  listRoot();
  const bool small = smallFileTest();
  const bool random = randomReadTest();
  SD.remove(kSmallFile);
  SD.remove(kBookFile);
  Serial.printf("RESULT: 4K %s, random-read %s. Test files removed.\n", small ? "OK" : "FAILED",
                random ? "OK" : "FAILED");
  Serial.println("Now press RST with the card still inserted: the board should boot again.");
}

void loop() { delay(1000); }
