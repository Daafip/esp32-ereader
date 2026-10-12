#include "bitmap.h"

#include <string.h>

void rotate1bpp(const uint8_t *src, int w, int h, uint8_t *dst, int degrees) {
  const int srcStride = (w + 7) / 8;
  const int dstStride = (h + 7) / 8;
  memset(dst, 0xFF, size_t(dstStride) * w);
  for (int y = 0; y < h; y++) {
    const uint8_t *row = src + y * srcStride;
    for (int x = 0; x < w; x++) {
      if (row[x >> 3] == 0xFF) {  // eight white pixels (text pages are mostly white)
        x |= 7;
        continue;
      }
      if (row[x >> 3] & (0x80 >> (x & 7))) continue;
      const int px = degrees == 90 ? y : h - 1 - y;
      const int py = degrees == 90 ? w - 1 - x : x;
      dst[py * dstStride + (px >> 3)] &= uint8_t(~(0x80 >> (px & 7)));
    }
  }
}
