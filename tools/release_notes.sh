#!/usr/bin/env bash
# Release notes for the firmware images from `pixi run fw-dist` (used by
# .github/workflows/release.yml). Usage: tools/release_notes.sh <version>
set -euo pipefail
v=$1
cat <<NOTES
Firmware $v for both readers. Nothing in this project has been tested on a board yet.

| Board | Flash over USB (at \`0x0\`) | Update over Wi-Fi |
|---|---|---|
| LilyGo T5-2.13 | \`t5-reader-$v.bin\` | \`t5-reader-$v-ota.bin\` |
| LilyGo T5-4.7" S3 | \`t5-47-reader-$v.bin\` | \`t5-47-reader-$v-ota.bin\` |

**Flash from a browser:** Chrome or Edge, <https://espressif.github.io/esptool-js/>,
Connect, Flash Address \`0x0\`, choose the \`.bin\` *without* \`-ota\`, Program.
T5-2.13: remove the SD card first. T5-4.7": if no port shows up, hold BOOT, press RST,
release BOOT.

**Update over Wi-Fi** (reader already running this firmware): Menu → Wi-Fi transfer,
open the page, "Update firmware", choose the \`-ota.bin\` for your board. The other
board's file is refused.

Diagnostic sketches for the T5-2.13 hardware checkpoints (\`docs/checkpoints.md\`):
\`t5-panel_tester-$v.bin\`, \`t5-button_test-$v.bin\`, \`t5-sd_test-$v.bin\`,
\`t5-calibration-$v.bin\`.

Books: drop an EPUB on the reader's Wi-Fi page (it typesets for the reader's screen),
or \`pixi run prep book.epub\` (\`--screen t5-47\` for the 4.7"). See \`docs/usage.md\`.
NOTES
