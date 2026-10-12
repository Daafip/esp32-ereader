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
# app only, for "Update firmware" on the Wi-Fi transfer page
cp firmware/.pio/build/reader/firmware.bin "out/firmware/t5-reader-$version-ota.bin"
ls -l out/firmware/*"$version".bin
echo "USB/browser: flash t5-<env>-$version.bin at 0x0. Over Wi-Fi: upload t5-reader-$version-ota.bin."
