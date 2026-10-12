#!/usr/bin/env bash
# Builds firmware/host/ui_preview (the real ui.cpp + bb_epaper as a RAM-only
# display) for the PC, renders every screen and converts them to PNG.
# Output: out/ui-preview/*.png and out/ui-preview/contact.png
set -euo pipefail
cd "$(dirname "$0")/.."

pio pkg install -d firmware -e reader >/dev/null
bb=firmware/.pio/libdeps/reader/bb_epaper/src
build=firmware/.pio/host
out=out/ui-preview
mkdir -p "$build" "$out"
c++ -std=gnu++17 -O1 -w \
  -I"$bb" -Ifirmware/include -Ifirmware/src -Ifirmware/lib/core/src \
  firmware/host/host_bb_epaper.cpp firmware/host/display_host.cpp firmware/host/ui_preview.cpp \
  firmware/src/ui.cpp firmware/lib/core/src/*.cpp \
  -o "$build/ui_preview"
rm -f "$out"/*.pbm "$out"/*.png
"$build/ui_preview" "$out" >/dev/null
python - "$out" <<'PY'
import sys
from pathlib import Path
from PIL import Image
from bookprep.render import contact_sheet, enlarge

out = Path(sys.argv[1])
framed = []
for pbm in sorted(out.glob("*.pbm")):
    img = Image.open(pbm)
    enlarge(img, 3).save(pbm.with_suffix(".png"))
    framed.append(enlarge(img, 2))
    pbm.unlink()
contact_sheet(framed, columns=3).save(out / "contact.png")
print(f"{len(framed)} screens -> {out}/")
PY
