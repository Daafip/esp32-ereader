#include "text_util.h"

#include <stdio.h>
#include <string.h>

namespace {

constexpr uint32_t kReplacement = 0xFFFD;

// U+00C0..U+00FF -> ASCII
const char *const kLatin1Upper[64] = {
    "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
    "D", "N", "O", "O", "O", "O", "O",  "x", "O", "U", "U", "U", "U", "Y", "Th", "ss",
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
    "d", "n", "o", "o", "o", "o", "o",  "/", "o", "u", "u", "u", "u", "y", "th", "y",
};

const char *foldCodepoint(uint32_t cp, char *one) {
  if (cp >= 0x20 && cp < 0x7F) {
    one[0] = char(cp);
    one[1] = '\0';
    return one;
  }
  if (cp >= 0xC0 && cp <= 0xFF) return kLatin1Upper[cp - 0xC0];
  switch (cp) {
    case 0x09: case 0x0A: case 0xA0: case 0x2002: case 0x2003: case 0x2009: case 0x202F:
      return " ";
    case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: case 0x2039: case 0x203A:
      return "'";
    case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033: case 0xAB: case 0xBB:
      return "\"";
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212:
      return "-";
    case 0x2026: return "...";
    case 0x2022: case 0xB7: return "*";
    case 0xA1: return "!";
    case 0xBF: return "?";
    case 0xB0: return "o";
    case 0xA9: return "(c)";
    case 0xAE: return "(R)";
    case 0x152: return "OE";
    case 0x153: return "oe";
    case 0x160: return "S";
    case 0x161: return "s";
    case 0x17D: return "Z";
    case 0x17E: return "z";
    case 0x178: return "Y";
    case 0x20AC: return "EUR";
    case 0xAD: case 0x200B: case 0xFEFF: return "";
    default: return "?";
  }
}

}  // namespace

size_t utf8Decode(const char *s, uint32_t &cp) {
  const uint8_t c = uint8_t(s[0]);
  if (c < 0x80) {
    cp = c;
    return 1;
  }
  int extra;
  if ((c & 0xE0) == 0xC0) {
    cp = c & 0x1F;
    extra = 1;
  } else if ((c & 0xF0) == 0xE0) {
    cp = c & 0x0F;
    extra = 2;
  } else if ((c & 0xF8) == 0xF0) {
    cp = c & 0x07;
    extra = 3;
  } else {
    cp = kReplacement;
    return 1;
  }
  for (int i = 1; i <= extra; i++) {
    const uint8_t cc = uint8_t(s[i]);
    if ((cc & 0xC0) != 0x80) {  // truncated sequence (also stops at the terminator)
      cp = kReplacement;
      return size_t(i);
    }
    cp = (cp << 6) | (cc & 0x3F);
  }
  return size_t(extra) + 1;
}

void asciiFold(const char *utf8, char *out, size_t outSize) {
  if (outSize == 0) return;
  size_t n = 0;
  char one[2];
  while (*utf8) {
    uint32_t cp;
    utf8 += utf8Decode(utf8, cp);
    const char *rep = foldCodepoint(cp, one);
    const size_t len = strlen(rep);
    if (n + len >= outSize) break;
    memcpy(out + n, rep, len);
    n += len;
  }
  out[n] = '\0';
}

bool fitText(const char *text, int maxWidth, MeasureFn measure, void *ctx, const char *ellipsis,
             char *out, size_t outSize) {
  if (outSize == 0) return false;
  const size_t len = strlen(text);
  if (len < outSize && measure(text, ctx) <= maxWidth) {
    memcpy(out, text, len + 1);
    return false;
  }
  // Code point boundaries, longest prefix first.
  const size_t ellLen = strlen(ellipsis);
  size_t cut = 0;
  size_t boundaries[256];
  size_t nb = 0;
  for (size_t i = 0; text[i] && nb < 256;) {
    uint32_t cp;
    i += utf8Decode(text + i, cp);
    boundaries[nb++] = i;
  }
  while (nb > 0) {
    cut = boundaries[--nb];
    if (cut + ellLen >= outSize) continue;
    memcpy(out, text, cut);
    // drop trailing spaces before the ellipsis
    size_t end = cut;
    while (end > 0 && out[end - 1] == ' ') end--;
    memcpy(out + end, ellipsis, ellLen + 1);
    if (measure(out, ctx) <= maxWidth) return true;
  }
  // Not even one character fits: just the ellipsis (or nothing).
  if (ellLen < outSize && measure(ellipsis, ctx) <= maxWidth) {
    memcpy(out, ellipsis, ellLen + 1);
  } else {
    out[0] = '\0';
  }
  return true;
}

int wrapText(const char *text, int maxWidth, MeasureFn measure, void *ctx, const char *ellipsis,
             char *lines, size_t lineSize, int maxLines) {
  int count = 0;
  while (*text == ' ') text++;
  while (*text && count < maxLines) {
    char *line = lines + size_t(count) * lineSize;
    if (count == maxLines - 1) {  // last line: whatever is left, cut if needed
      fitText(text, maxWidth, measure, ctx, ellipsis, line, lineSize);
      return count + 1;
    }
    // longest run of whole words that fits
    size_t best = 0;
    for (size_t end = 0;; end++) {
      if (text[end] == ' ' || text[end] == '\0') {
        if (end >= lineSize) break;
        memcpy(line, text, end);
        line[end] = '\0';
        if (measure(line, ctx) > maxWidth) break;
        best = end;
        if (text[end] == '\0') break;
      }
    }
    if (best == 0) {  // a single word wider than the line: cut it
      fitText(text, maxWidth, measure, ctx, ellipsis, line, lineSize);
      const char *space = strchr(text, ' ');
      text = space ? space : text + strlen(text);
    } else {
      memcpy(line, text, best);
      line[best] = '\0';
      text += best;
    }
    while (*text == ' ') text++;
    count++;
  }
  return count;
}

void formatProgress(uint32_t page, uint32_t pageCount, char *out, size_t outSize) {
  if (pageCount == 0) {
    snprintf(out, outSize, "-");
    return;
  }
  const unsigned pct = unsigned(uint64_t(page + 1) * 100 / pageCount);
  snprintf(out, outSize, "%u/%u  \xC2\xB7  %u%%", unsigned(page + 1), unsigned(pageCount), pct);
}

void formatBytes(uint64_t bytes, char *out, size_t outSize) {
  if (bytes >= (uint64_t(1) << 30)) {
    const uint64_t tenths = (bytes * 10 + (uint64_t(1) << 29)) >> 30;
    snprintf(out, outSize, "%u.%u GB", unsigned(tenths / 10), unsigned(tenths % 10));
  } else if (bytes >= (uint64_t(1) << 20)) {
    snprintf(out, outSize, "%u MB", unsigned((bytes + (1u << 19)) >> 20));
  } else {
    snprintf(out, outSize, "%u KB", unsigned((bytes + 512) >> 10));
  }
}
