#!/usr/bin/env bash
# Builds every firmware env and collects one merged image per env (bootloader,
# partition table and app in one file) in out/firmware/, for flashing from a
# browser without PlatformIO:
#   Chrome or Edge -> https://espressif.github.io/esptool-js/ -> Connect ->
#   Flash Address 0x0 -> choose out/firmware/t5-reader-<version>.bin -> Program
set -euo pipefail
cd "$(dirname "$0")/.."

pio run -d firmware >/dev/null
version=$(sed -n 's/.*-DFW_VERSION=\\"\([^\\]*\)\\".*/\1/p' firmware/platformio.ini)
mkdir -p out/firmware
for env in reader calibration panel_tester button_test sd_test; do
  cp "firmware/.pio/build/$env/firmware.factory.bin" "out/firmware/t5-$env-$version.bin"
done
# The other boards' readers: merged image + app-only image ("Update firmware" on
# the Wi-Fi page) each. The reader refuses another chip's image (ESP32, -S3, -C6).
cp firmware/.pio/build/reader/firmware.bin "out/firmware/t5-reader-$version-ota.bin"
for pair in reader_t547:t5-47 reader_m5papers3:m5papers3 reader_m5paper:m5paper \
    reader_firebeetle75:firebeetle-75; do
  env=${pair%%:*}
  name=${pair#*:}
  cp "firmware/.pio/build/$env/firmware.factory.bin" "out/firmware/$name-reader-$version.bin"
  cp "firmware/.pio/build/$env/firmware.bin" "out/firmware/$name-reader-$version-ota.bin"
done
# The 7.5" board's hardware test (docs/checkpoints.md)
cp firmware/.pio/build/fb75_test/firmware.factory.bin "out/firmware/firebeetle-75-test-$version.bin"
ls -l out/firmware/*"$version"*.bin
echo "USB/browser: flash <board>-reader-$version.bin (T5-2.13: t5-reader-$version.bin) at 0x0."
echo "Over Wi-Fi: upload the matching -ota.bin."
