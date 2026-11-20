#include "position_sync.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "catalog.h"
#include "t5b_format.h"

namespace possync {
namespace {

// [lo, hi) of contents entry c (-1: the pages before the first) in whole-book pages.
void span(const Layout &l, int c, uint32_t &lo, uint32_t &hi) {
  lo = c >= 0 ? l.starts[c] : 0;
  hi = c + 1 < l.tocCount ? l.starts[c + 1] : l.wholePages;
  if (hi > l.wholePages) hi = l.wholePages;
  if (hi <= lo) hi = lo + 1;  // contents out of order: an entry of one page
}

// k/n in millionths, rounded up: read back with floor(ppm * n / 1e6) it is k again.
uint32_t ceilPpm(uint64_t k, uint64_t n) {
  if (!n) return 0;
  const uint64_t v = (k * kMillion + n - 1) / n;
  return v > kMillion ? kMillion : uint32_t(v);
}

bool hasContents(const Layout &l) { return l.tocCount > 0 && l.starts; }

void clean(std::string &out, const char *text) {
  for (const char *p = text; *p; ++p) out += (*p == '\t' || *p == '\n' || *p == '\r') ? ' ' : *p;
}

void appendNumber(std::string &out, long long v) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%lld", v);
  out += buf;
}

// The record's fields after its first one: rev ppm chapter chapterPpm toc device name title.
void appendFields(std::string &out, const Record &r) {
  appendNumber(out, r.rev);
  out += '\t';
  appendNumber(out, r.pos.ppm);
  out += '\t';
  appendNumber(out, r.pos.chapter);
  out += '\t';
  appendNumber(out, r.pos.chapterPpm);
  out += '\t';
  appendNumber(out, r.pos.tocCount);
  out += '\t';
  clean(out, r.device);
  out += '\t';
  clean(out, r.name);
  out += '\t';
  clean(out, r.title);
}

// f[0..7] as appendFields() writes them.
bool readFields(char **f, int n, Record &r) {
  if (n < 6) return false;
  char *end = nullptr;
  r = Record();
  r.rev = uint32_t(strtoul(f[0], &end, 10));
  if (end == f[0]) return false;
  r.pos.ppm = uint32_t(strtoul(f[1], nullptr, 10));
  if (r.pos.ppm > kMillion) r.pos.ppm = kMillion;
  r.pos.chapter = int32_t(strtol(f[2], nullptr, 10));
  r.pos.chapterPpm = uint32_t(strtoul(f[3], nullptr, 10));
  if (r.pos.chapterPpm > kMillion) r.pos.chapterPpm = kMillion;
  const unsigned long toc = strtoul(f[4], nullptr, 10);
  r.pos.tocCount = uint16_t(toc > 0xFFFF ? 0 : toc);
  if (r.pos.chapter < -1 || r.pos.chapter >= int32_t(r.pos.tocCount)) {
    r.pos.chapter = -1;  // makes no sense: the fraction of the book only
    r.pos.tocCount = 0;
  }
  snprintf(r.device, sizeof(r.device), "%s", f[5]);
  if (n > 6) snprintf(r.name, sizeof(r.name), "%s", f[6]);
  if (n > 7) snprintf(r.title, sizeof(r.title), "%s", f[7]);
  return true;
}

// Calls fn(line) for every line of text (a copy, so it can be cut in place).
template <typename F>
void eachLine(const char *text, size_t len, F fn) {
  std::string copy(text, len);
  char *p = &copy[0];
  char *end = p + copy.size();
  while (p < end) {
    char *nl = static_cast<char *>(memchr(p, '\n', size_t(end - p)));
    if (nl) *nl = '\0';
    fn(p);
    if (!nl) break;
    p = nl + 1;
  }
}

}  // namespace

void bookKey(const char *stem, char out[9]) {
  uint32_t h = 2166136261u;
  for (const char *p = stem; *p; ++p) {
    h ^= uint8_t(tolower(uint8_t(*p)));
    h *= 16777619u;
  }
  snprintf(out, 9, "%08x", unsigned(h));
}

bool parseBroker(const char *text, char *host, size_t hostSize, uint16_t &port) {
  if (!text || !hostSize) return false;
  host[0] = '\0';
  while (*text == ' ') text++;
  if (strncasecmp(text, "mqtt://", 7) == 0) text += 7;
  if (strstr(text, "://")) return false;  // mqtts://, ws://: not spoken here
  size_t n = strlen(text);
  while (n && (text[n - 1] == ' ' || text[n - 1] == '/')) n--;
  const char *colon = static_cast<const char *>(memchr(text, ':', n));
  const size_t hostLen = colon ? size_t(colon - text) : n;
  if (!hostLen || hostLen >= hostSize || memchr(text, '/', n)) return false;
  for (size_t i = 0; i < hostLen; i++) {
    if (text[i] == ' ' || text[i] == '@') return false;
  }
  port = kDefaultPort;
  if (colon) {
    const char *digits = colon + 1;
    const size_t m = n - hostLen - 1;
    if (!m || m > 5) return false;
    unsigned long v = 0;
    for (size_t i = 0; i < m; i++) {
      if (!isdigit(uint8_t(digits[i]))) return false;
      v = v * 10 + unsigned(digits[i] - '0');
    }
    if (!v || v > 65535) return false;
    port = uint16_t(v);
  }
  memcpy(host, text, hostLen);
  host[hostLen] = '\0';
  return true;
}

bool partRange(const char *title, uint32_t &first, uint32_t &last) {
  const size_t n = strlen(title);
  if (n < 7 || title[n - 1] != ')') return false;
  const char *open = nullptr;
  for (const char *p = title + n - 1; p > title; --p) {
    if (*p == '(') {
      open = p;
      break;
    }
  }
  if (!open || open == title || open[-1] != ' ') return false;
  char *end = nullptr;
  const char *p = open + 1;
  if (!isdigit(uint8_t(*p))) return false;
  first = uint32_t(strtoul(p, &end, 10));
  if (strncmp(end, "\xE2\x80\x93", 3) != 0 || !isdigit(uint8_t(end[3]))) return false;
  p = end + 3;
  last = uint32_t(strtoul(p, &end, 10));
  return end == title + n - 1 && first >= 1 && last >= first;
}

Position positionOf(const Layout &l, uint32_t w) {
  Position p;
  if (!l.wholePages) return p;
  if (w >= l.wholePages) w = l.wholePages - 1;
  p.ppm = ceilPpm(w, l.wholePages);
  if (hasContents(l)) {
    p.chapter = t5b::chapterForPage(l.starts, l.tocCount, w);
    uint32_t lo, hi;
    span(l, p.chapter, lo, hi);
    p.chapterPpm = w >= lo ? ceilPpm(w - lo, hi - lo) : 0;
    p.tocCount = uint16_t(l.tocCount > 0xFFFF ? 0 : l.tocCount);
    if (!p.tocCount) p.chapter = -1;
  }
  return p;
}

uint32_t wholePageFor(const Layout &l, const Position &p) {
  if (!l.wholePages) return 0;
  uint64_t w;
  if (p.tocCount > 0 && hasContents(l) && p.tocCount == l.tocCount && p.chapter >= -1 &&
      p.chapter < l.tocCount) {
    uint32_t lo, hi;
    span(l, p.chapter, lo, hi);
    w = lo + uint64_t(p.chapterPpm > kMillion ? kMillion : p.chapterPpm) * (hi - lo) / kMillion;
    if (w >= hi) w = hi - 1;
  } else {
    w = uint64_t(p.ppm > kMillion ? kMillion : p.ppm) * l.wholePages / kMillion;
  }
  return w < l.wholePages ? uint32_t(w) : l.wholePages - 1;
}

bool contains(const Layout &l, uint32_t wholePage) {
  return wholePage >= l.first && wholePage - l.first < l.pages;
}

std::string formatRecord(const Record &r) {
  std::string out = kProtocol;
  out += "\nposition\t";
  appendFields(out, r);
  out += '\n';
  return out;
}

bool parseRecord(const char *text, size_t len, Record &out) {
  bool header = false, found = false;
  eachLine(text, len, [&](char *line) {
    char *f[9];
    const int n = catalog::splitFields(line, f, 9);
    if (!header) {
      header = n == 2 && strcmp(f[0], "T5SYNC") == 0 && strcmp(f[1], "1") == 0;
      return;
    }
    if (!found && n >= 7 && strcmp(f[0], "position") == 0) found = readFields(f + 1, n - 1, out);
  });
  return header && found;
}

std::string formatState(const BookState &s) {
  std::string out;
  if (s.synced) {
    out += "synced\t";
    appendNumber(out, s.rev);
    out += '\t';
    appendNumber(out, s.page);
    out += '\t';
    appendNumber(out, s.pages);
    out += '\n';
  }
  if (s.dirty) out += "dirty\n";
  if (s.part) {
    out += "part\t";
    appendNumber(out, s.first);
    out += '\t';
    appendNumber(out, s.wholePages);
    out += '\n';
    for (size_t i = 0; i < s.starts.size(); i += 16) {
      out += "toc";
      for (size_t j = i; j < s.starts.size() && j < i + 16; j++) {
        out += '\t';
        appendNumber(out, s.starts[j]);
      }
      out += '\n';
    }
  }
  if (s.hasRemote) {
    out += "remote\t";
    out += s.ask ? "ask" : s.elsewhere ? "elsewhere" : "-";
    out += '\t';
    appendFields(out, s.remote);
    out += '\n';
  }
  return out;
}

void parseState(const char *text, size_t len, BookState &out) {
  out = BookState();
  eachLine(text, len, [&](char *line) {
    if (strncmp(line, "toc\t", 4) == 0) {
      if (!out.part) return;
      for (char *p = line + 4; *p;) {
        char *end = nullptr;
        const unsigned long v = strtoul(p, &end, 10);
        if (end == p) break;
        if (out.starts.size() < 0xFFFF) out.starts.push_back(uint32_t(v));
        p = end;
        while (*p == '\t' || *p == '\r') p++;
      }
      return;
    }
    char *f[10];
    const int n = catalog::splitFields(line, f, 10);
    if (n >= 4 && strcmp(f[0], "synced") == 0) {
      out.synced = true;
      out.rev = uint32_t(strtoul(f[1], nullptr, 10));
      out.page = uint32_t(strtoul(f[2], nullptr, 10));
      out.pages = uint32_t(strtoul(f[3], nullptr, 10));
    } else if (n >= 1 && strcmp(f[0], "dirty") == 0) {
      out.dirty = true;
    } else if (n >= 3 && strcmp(f[0], "part") == 0) {
      out.part = true;
      out.first = uint32_t(strtoul(f[1], nullptr, 10));
      out.wholePages = uint32_t(strtoul(f[2], nullptr, 10));
      if (out.wholePages <= out.first) out.part = false;  // makes no sense
    } else if (n >= 8 && strcmp(f[0], "remote") == 0 && readFields(f + 2, n - 2, out.remote)) {
      out.hasRemote = true;
      out.ask = strcmp(f[1], "ask") == 0;
      out.elsewhere = strcmp(f[1], "elsewhere") == 0;
    }
  });
  if (!out.part) {
    out.starts.clear();
    out.first = out.wholePages = 0;
  }
}

Action decide(const BookState &s, uint32_t local, uint32_t wholePages, const Record *remote,
              uint32_t mapped, const char *device) {
  if (!remote) return s.synced || s.dirty || local > 0 ? Action::Publish : Action::Nothing;
  if (strcmp(remote->device, device) == 0) {  // written by this reader
    return !s.dirty && mapped == local ? Action::Seen : Action::Publish;
  }
  if (!s.synced || remote->rev > s.rev) {  // another reader went on since the last sync
    if (s.ask && s.hasRemote && remote->rev == s.remote.rev) return Action::Nothing;  // asked
    return mapped == local ? Action::Seen : Action::Ask;
  }
  if (s.ask) return Action::Nothing;  // the answer decides
  if (remote->rev < s.rev) return Action::Publish;  // the broker lost what was sent
  const bool moved = s.dirty || s.page != local || s.pages != wholePages;
  return moved ? Action::Publish : Action::Nothing;
}

uint32_t newRev(const BookState &s, const Record *remote) {
  uint32_t r = s.rev;
  if (s.hasRemote && s.remote.rev > r) r = s.remote.rev;
  if (remote && remote->rev > r) r = remote->rev;
  return r + 1;
}

void markSynced(BookState &s, uint32_t rev, uint32_t page, uint32_t pages) {
  s.synced = true;
  s.rev = rev;
  s.page = page;
  s.pages = pages;
  s.dirty = false;
  s.ask = false;
  s.elsewhere = false;
}

void markAsk(BookState &s, const Record &remote) {
  s.hasRemote = true;
  s.remote = remote;
  s.ask = true;
  s.elsewhere = false;
}

void markStay(BookState &s) {
  if (s.hasRemote && s.remote.rev > s.rev) s.rev = s.remote.rev;
  s.synced = true;
  s.dirty = true;
  s.ask = false;
  s.elsewhere = false;
}

void markElsewhere(BookState &s, uint32_t page, uint32_t pages) {
  const uint32_t rev = s.hasRemote && s.remote.rev > s.rev ? s.remote.rev : s.rev;
  markSynced(s, rev, page, pages);
  s.elsewhere = s.hasRemote;
}

namespace {
void askAgainIfElsewhere(BookState &s) {
  if (!s.elsewhere) return;
  s.elsewhere = false;
  s.ask = s.hasRemote;
}
}  // namespace

void setWhole(BookState &s) {
  s.part = false;
  s.first = s.wholePages = 0;
  s.starts.clear();
  askAgainIfElsewhere(s);
}

void setPart(BookState &s, uint32_t first, uint32_t wholePages,
             const std::vector<uint32_t> &starts) {
  s.part = wholePages > first;
  s.first = s.part ? first : 0;
  s.wholePages = s.part ? wholePages : 0;
  s.starts = s.part ? starts : std::vector<uint32_t>();
  askAgainIfElsewhere(s);
}

bool shouldTry(Backoff &b) {
  if (!b.skip) return true;
  b.skip--;
  return false;
}

void failed(Backoff &b) {
  if (b.failures < 5) b.failures++;
  b.skip = uint8_t(1u << (b.failures - 1));
}

void succeeded(Backoff &b) {
  b.failures = 0;
  b.skip = 0;
}

}  // namespace possync
