#!/usr/bin/env bash
# Builds firmware/host/ui_preview (the real ui.cpp + bb_epaper as a RAM-only
# display) for the PC once per board, renders every screen and converts them to PNG.
# Output: out/ui-preview/<board>/*.png and contact.png, for
#   t5-213     LilyGo T5-2.13 (250x122, two buttons)
#   t5-47      LilyGo T5-4.7" S3 (540x960, one button)
#   m5paper    M5PaperS3 / M5Paper (540x960, touch: bigger rows, footer buttons)
#   firebeetle-75, firebeetle-75-landscape
#              FireBeetle 2 ESP32-C6 + 7.5" (480x800 portrait, 800x480 landscape, three buttons)
set -euo pipefail
cd "$(dirname "$0")/.."

pio pkg install -d firmware -e reader >/dev/null
bb=firmware/.pio/libdeps/reader/bb_epaper/src
build=firmware/.pio/host
out=out/ui-preview
mkdir -p "$build" "$out"
rm -f "$out"/*.png  # previews from before the per-board folders
cc -std=c99 -O1 -w -c firmware/lib/qrcodegen/src/qrcodegen.c -o "$build/qrcodegen.o"

# board:screen:define:orientation (Settings -> Orientation; 2 = landscape on the 7.5")
for spec in t5-213:t5-213:: t5-47:4.7in:-DBOARD_T5_47_S3: m5paper:4.7in:-DBOARD_M5PAPER_S3: \
    firebeetle-75:7.5in:-DBOARD_FIREBEETLE_75:0 \
    firebeetle-75-landscape:7.5in-landscape:-DBOARD_FIREBEETLE_75:2; do
  IFS=: read -r board screen define orientation <<<"$spec"
  python -m bookprep.devpages --screen "$screen" --t5b "$build/sample-$screen.t5b" \
    --picture "$build/sample-$screen-picture.bin" >/dev/null
  c++ -std=gnu++17 -O1 -w $define \
    -I"$bb" -Ifirmware/include -Ifirmware/src -Ifirmware/lib/core/src -Ifirmware/lib/qrcodegen/src \
    firmware/host/host_bb_epaper.cpp firmware/host/display_host.cpp firmware/host/ui_preview.cpp \
    firmware/src/ui.cpp firmware/lib/core/src/*.cpp "$build/qrcodegen.o" \
    -o "$build/ui_preview-$board"
  mkdir -p "$out/$board"
  rm -f "$out/$board"/*.pbm "$out/$board"/*.png
  "$build/ui_preview-$board" "$out/$board" "$build/sample-$screen.t5b" \
    "$build/sample-$screen-picture.bin" $orientation >/dev/null
done

python - "$out" <<'PY'
import sys
from pathlib import Path
from PIL import Image
from bookprep.render import contact_sheet, enlarge

for out in sorted(p for p in Path(sys.argv[1]).iterdir() if p.is_dir()):
    framed = []
    for pbm in sorted(out.glob("*.pbm")):
        img = Image.open(pbm)
        scale = 3 if img.width < 400 else 1  # small panel: enlarge to judge pixels
        enlarge(img, scale).save(pbm.with_suffix(".png"))
        framed.append(enlarge(img, 2 if img.width < 400 else 1))
        pbm.unlink()
    contact_sheet(framed, columns=3 if framed[0].width < 800 else 4).save(out / "contact.png")
    print(f"{len(framed)} screens -> {out}/")
PY
