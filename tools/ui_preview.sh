#!/usr/bin/env bash
# Builds firmware/host/ui_preview (the real ui.cpp + bb_epaper as a RAM-only
# display) for the PC once per board, renders every screen and converts them to PNG.
# Output: out/ui-preview/<screen>/*.png and contact.png, for
#   t5-213  LilyGo T5-2.13 (250x122)
#   t5-47   LilyGo T5-4.7" S3 (540x960, -DBOARD_T5_47_S3)
set -euo pipefail
cd "$(dirname "$0")/.."

pio pkg install -d firmware -e reader >/dev/null
bb=firmware/.pio/libdeps/reader/bb_epaper/src
build=firmware/.pio/host
out=out/ui-preview
mkdir -p "$build" "$out"
rm -f "$out"/*.png  # previews from before the per-board folders
cc -std=c99 -O1 -w -c firmware/lib/qrcodegen/src/qrcodegen.c -o "$build/qrcodegen.o"

for board in t5-213:"" t5-47:-DBOARD_T5_47_S3; do
  screen=${board%%:*}
  define=${board#*:}
  python -m bookprep.devpages --screen "$screen" --t5b "$build/sample-$screen.t5b" >/dev/null
  c++ -std=gnu++17 -O1 -w $define \
    -I"$bb" -Ifirmware/include -Ifirmware/src -Ifirmware/lib/core/src -Ifirmware/lib/qrcodegen/src \
    firmware/host/host_bb_epaper.cpp firmware/host/display_host.cpp firmware/host/ui_preview.cpp \
    firmware/src/ui.cpp firmware/lib/core/src/*.cpp "$build/qrcodegen.o" \
    -o "$build/ui_preview-$screen"
  mkdir -p "$out/$screen"
  rm -f "$out/$screen"/*.pbm "$out/$screen"/*.png
  "$build/ui_preview-$screen" "$out/$screen" "$build/sample-$screen.t5b" >/dev/null
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
