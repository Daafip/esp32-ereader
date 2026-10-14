// UTF-8 helpers for the small screen.
#pragma once

#include <stddef.h>
#include <stdint.h>

// Decodes one UTF-8 sequence at `s` (must not be at the terminator).
// Returns the bytes consumed (>= 1); invalid bytes decode to U+FFFD.
size_t utf8Decode(const char *s, uint32_t &cp);

// Transliterates UTF-8 to printable ASCII (é -> e, “ -> ", … -> ...) for
// bb_epaper's built-in fonts, which only have ASCII. Always NUL-terminates.
void asciiFold(const char *utf8, char *out, size_t outSize);

// Width function for fitText(): pixel width of a NUL-terminated string.
using MeasureFn = int (*)(const char *text, void *ctx);

// Copies the longest prefix of `text` (on a code point boundary) that fits in
// `maxWidth`; if text had to be cut, `ellipsis` is appended and still fits.
// Returns true if the text was cut.
bool fitText(const char *text, int maxWidth, MeasureFn measure, void *ctx, const char *ellipsis,
             char *out, size_t outSize);

// Greedy word wrap into at most maxLines lines of lineSize bytes each. If text
// is left over, the last line is cut with `ellipsis`. Returns the line count.
int wrapText(const char *text, int maxWidth, MeasureFn measure, void *ctx, const char *ellipsis,
             char *lines, size_t lineSize, int maxLines);

// "27/3596  ·  1%" (UTF-8 middle dot; page is 0-based, shown 1-based)
void formatProgress(uint32_t page, uint32_t pageCount, char *out, size_t outSize);

// "6.2 GB", "740 MB", "12 KB" (binary units, rounded; GB with one decimal)
void formatBytes(uint64_t bytes, char *out, size_t outSize);
