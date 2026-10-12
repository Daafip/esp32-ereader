// 1-bpp bitmaps in the .t5b page layout: MSB first, 1 = white, rows padded to
// whole bytes.
#pragma once

#include <stdint.h>

// Rotates the w x h bitmap `src` into `dst` (h x w), the way bb_epaper and
// FastEPD map a canvas drawn with setRotation(degrees) onto the panel:
//   90:  canvas (x, y) -> panel (y, w - 1 - x)
//   270: canvas (x, y) -> panel (h - 1 - y, x)
// `dst` must hold ((h + 7) / 8) * w bytes; padding bits come out white.
void rotate1bpp(const uint8_t *src, int w, int h, uint8_t *dst, int degrees);
