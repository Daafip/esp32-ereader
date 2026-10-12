#!/usr/bin/env bash
# Converts the UI fonts with bb_epaper's own fontconvert, built from the exact
# library version the firmware uses (firmware/.pio/libdeps). Output: compressed
# bitmap fonts covering codepage 1252 (32..255: ASCII, é ë ï ü, curly quotes,
# dashes, ellipsis), committed in firmware/include/fonts/.
# Run through pixi (needs $CONDA_PREFIX for FreeType): pixi run ui-fonts
set -euo pipefail
cd "$(dirname "$0")/.."

pio pkg install -d firmware -e reader >/dev/null
src=firmware/.pio/libdeps/reader/bb_epaper/fontconvert/main.c
bin=firmware/.pio/fontconvert
cc -O2 -w -I"$CONDA_PREFIX/include/freetype2" "$src" \
  -L"$CONDA_PREFIX/lib" -lfreetype -Wl,-rpath,"$CONDA_PREFIX/lib" -o "$bin"

fonts="$CONDA_PREFIX/fonts"
out=firmware/include/fonts
mkdir -p "$out"
rm -f "$out"/*.h
# fontconvert sizes are points at 141 dpi: 5 pt ~ 10 px, 6 pt ~ 12 px, 7 pt ~ 14 px, 9 pt ~ 18 px.
# One set per UI scale (kUiScale in the board header): 1 = T5-2.13, 2 = T5-4.7".
for scale in 1 2; do
  suffix=$([ "$scale" = 1 ] && echo "" || echo "_${scale}x")
  "$bin" "$fonts/Ubuntu-M.ttf" "$out/ui_small$suffix.h" $((5 * scale)) 32 255    # status bar, footer, details
  "$bin" "$fonts/Ubuntu-R.ttf" "$out/ui_body$suffix.h" $((6 * scale)) 32 255     # list items, body text
  "$bin" "$fonts/Ubuntu-B.ttf" "$out/ui_title$suffix.h" $((7 * scale)) 32 255    # headers, headlines
  "$bin" "$fonts/Ubuntu-M.ttf" "$out/ui_display$suffix.h" $((9 * scale)) 32 255  # sleep screen title, big values
done
