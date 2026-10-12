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

font="$CONDA_PREFIX/fonts/Ubuntu-R.ttf"
out=firmware/include/fonts
mkdir -p "$out"
# fontconvert sizes are points at 141 dpi: 6 pt ~ 12 px, 8 pt ~ 16 px
"$bin" "$font" "$out/ui_font_12.h" 6 32 255
"$bin" "$font" "$out/ui_font_16.h" 8 32 255
