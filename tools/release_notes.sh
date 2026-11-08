#!/usr/bin/env bash
# Release notes for the firmware images from `pixi run fw-dist` (used by
# .github/workflows/release.yml). Usage: tools/release_notes.sh <version>
set -euo pipefail
v=$1
# FWSWITCH_SIGNED=1: the APK has the release key (release.yml), so updates install over it
signing="This build is signed with a debug key: if Android won't install it over an older copy, uninstall that first (the app then forgets its two firmware files)."
[ -n "${FWSWITCH_SIGNED:-}" ] && signing="Signed with the project's release key: later versions install over it."
# FDROID_FINGERPRINT (release.yml): the app's F-Droid repository on GitHub Pages
fdroid=""
[ -n "${FDROID_FINGERPRINT:-}" ] && fdroid="Or get it, and its updates, through F-Droid: add the repository
<https://daafip.github.io/esp32-ereader/fdroid/repo?fingerprint=$FDROID_FINGERPRINT>
(or open <https://daafip.github.io/esp32-ereader/fdroid/repo/> on the phone)."
cat <<NOTES
Firmware $v for every supported reader. **Tested on hardware: the LilyGo T5-2.13
(V2.3.1) and the home-built FireBeetle 7.5" without a card;** the FireBeetle's card
build and the T5-4.7" S3, M5PaperS3 and M5Paper builds have not run on a board yet
(\`docs/checkpoints.md\` lists what to check; reports welcome).

| Board | Flash over USB (at \`0x0\`) | Update over Wi-Fi |
|---|---|---|
| LilyGo T5-2.13 | \`t5-reader-$v.bin\` | \`t5-reader-$v-ota.bin\` |
| LilyGo T5-4.7" S3 | \`t5-47-reader-$v.bin\` | \`t5-47-reader-$v-ota.bin\` |
| M5Stack M5PaperS3 | \`m5papers3-reader-$v.bin\` | \`m5papers3-reader-$v-ota.bin\` |
| M5Stack M5Paper | \`m5paper-reader-$v.bin\` | \`m5paper-reader-$v-ota.bin\` |
| FireBeetle 2 ESP32-C6 + 7.5" | \`firebeetle-75-reader-$v.bin\` | \`firebeetle-75-reader-$v-ota.bin\` |
| the same without a microSD card | \`firebeetle-75-flash-reader-$v.bin\` | over USB only |

**Install from a browser:** Chrome or Edge, <https://daafip.github.io/esp32-ereader/>,
pick your board, Install. Leave "Erase everything" unticked to keep settings, bookmarks
and reading positions (books on the SD card are never touched).
T5-2.13: remove the SD card first. T5-4.7" / M5PaperS3 / FireBeetle: if no port shows up,
hold BOOT, press RST, release BOOT (M5PaperS3: see M5Stack's download-mode instructions).

Or with <https://espressif.github.io/esptool-js/>: Flash Address \`0x0\`, the \`.bin\`
*without* \`-ota\`. This image erases the reader's settings (not the books).

**Update over Wi-Fi** (reader already running this firmware): Menu → Wi-Fi transfer,
open the page, "Update firmware", choose the \`-ota.bin\` for your board. A file for
another chip (ESP32, ESP32-S3, ESP32-C6) is refused, and so is another board's file.

**Android app** \`fw-switch-$v.apk\` (Android 10 or later): puts this reader or
ESPHome on the FireBeetle 7.5" over a USB-C OTG cable, without a PC. Open the file on
the phone and allow installing apps from that source. See \`docs/firmware-switcher.md\`.
$signing
$fdroid

Diagnostic sketches for the T5-2.13 hardware checkpoints (\`docs/checkpoints.md\`):
\`t5-panel_tester-$v.bin\`, \`t5-button_test-$v.bin\`, \`t5-sd_test-$v.bin\`,
\`t5-calibration-$v.bin\`; for the FireBeetle 7.5": \`firebeetle-75-test-$v.bin\`.

Books: drop an EPUB on the reader's Wi-Fi page (it typesets for the reader's screen),
or \`pixi run prep book.epub\` (\`--screen 4.7in\` for the 4.7" readers, \`7.5in\` or
\`7.5in-landscape\` for the FireBeetle). Since 2.0.0 books are compressed (\`.t5b\`
version 2, about 5 times smaller): they need firmware 2.0.0 or later, and older books
still open (\`prep --uncompressed\` makes books for 1.x firmware). A FireBeetle without a
card keeps its books in its flash, room for about 170 pages: of a bigger book the page
sends the part you choose. See \`docs/usage.md\`.
NOTES
