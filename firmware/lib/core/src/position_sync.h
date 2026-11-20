// Reading position sync (docs/progress-sync.md): the MQTT broker keeps one retained
// record per book, t5reader/progress/<key>, which every reader reads and writes. Here,
// without Arduino so it can be tested on the PC: the book's key, where a page lies in a
// book that another reader typeset for another screen, the record's text, what to do
// with a book in a sync, the state kept per book on the card, and how long to leave
// automatic syncs alone after a failed join. The network and the files: src/syncer.cpp.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace possync {

constexpr const char *kTopicPrefix = "t5reader/progress/";  // + key
constexpr const char *kProtocol = "T5SYNC\t1";              // first line of every record
constexpr uint16_t kDefaultPort = 1883;
constexpr uint32_t kMillion = 1000000;

// The book's key: FNV-1a over its file stem, lower-cased, as 8 hex digits. The page and
// the book server both name a book after its source file, so one EPUB has one key on
// every reader, whatever screen it was typeset for (a part has the whole book's name).
void bookKey(const char *stem, char out[9]);

// Broker address: "host", "host:1883" or "mqtt://host:1883" (port 1883 if none).
// False for anything else (mqtts://, a path, an empty host).
bool parseBroker(const char *text, char *host, size_t hostSize, uint16_t &port);

// " (152–297)" at the end of a title, as bookprep's part_title() writes it (en dash):
// the first and last page of a part, 1-based. Such a book can't be synced without
// knowing the whole book (BookState::part).
bool partRange(const char *title, uint32_t &first, uint32_t &last);

// Where this reader's copy lies in the whole book: all of it (first 0, wholePages ==
// pages), or a part.
struct Layout {
  uint32_t pages = 0;                // pages of the file on the reader
  uint32_t first = 0;                // whole-book page of its page 0
  uint32_t wholePages = 0;           // pages of the whole book
  const uint32_t *starts = nullptr;  // the whole book's contents: first page of each entry
  int tocCount = 0;                  // 0: none known (then the fraction of the book only)
};

// A place in the book that means the same on every screen.
struct Position {
  uint32_t ppm = 0;         // start of the page in millionths of the whole book (rounded up)
  int32_t chapter = -1;     // contents entry it is in (-1: before the first)
  uint32_t chapterPpm = 0;  // start of the page in millionths of that entry's pages
  uint16_t tocCount = 0;    // entries in the whole book's contents (0: chapter not known)
};

// Where whole-book page `wholePage` starts.
Position positionOf(const Layout &l, uint32_t wholePage);
// The whole-book page in this layout that holds `p`: by its chapter when this copy's
// contents have as many entries as the sender's (measured 2026-11-15: within a page
// T5-2.13 -> 7.5", 1.8 pages early on average 7.5" -> T5-2.13), else by the fraction
// of the book (2-14 pages off).
uint32_t wholePageFor(const Layout &l, const Position &p);
// Whether a whole-book page is in this file (then it is page wholePage - first here).
bool contains(const Layout &l, uint32_t wholePage);

// One record on the broker.
struct Record {
  uint32_t rev = 0;    // goes up by one with every new position, whichever reader writes it
  Position pos;
  char device[9] = "";  // the reader that wrote it: 8 hex digits from its MAC
  char name[33] = "";   // its name, for the question
  char title[65] = "";  // the book's (for people looking at the broker)
};
// "T5SYNC\t1\nposition\t<rev>\t<ppm>\t<chapter>\t<chapterPpm>\t<toc>\t<device>\t<name>\t<title>\n"
// Tabs and line breaks in the texts become spaces.
std::string formatRecord(const Record &r);
// False if it isn't a record of this version.
bool parseRecord(const char *text, size_t len, Record &out);

// What this reader knows about a book: kept on the card, /books/.sync/<stem>.txt.
struct BookState {
  // The last sync: the record's rev then, and this reader's place (whole-book page).
  bool synced = false;
  uint32_t rev = 0;
  uint32_t page = 0;
  uint32_t pages = 0;  // the whole book's pages then
  // Answered "stay" to another reader's place: this one goes to the broker next time.
  bool dirty = false;
  // The file is a part: its first page in the whole book (0-based), the whole book's
  // pages and contents (empty if the book server didn't send them).
  bool part = false;
  uint32_t first = 0;
  uint32_t wholePages = 0;
  std::vector<uint32_t> starts;
  // The last record from another reader, and whether its question is still open
  // (ask), or it was in a part not on the reader (elsewhere: asked again when
  // another part of the book arrives).
  bool hasRemote = false;
  bool ask = false;
  bool elsewhere = false;
  Record remote;
};
std::string formatState(const BookState &s);
void parseState(const char *text, size_t len, BookState &out);  // unknown lines skipped

// What a sync does with a book. `local`: this reader's whole-book page, `wholePages`
// the whole book's; `remote`: the record on the broker (nullptr: none) and `mapped`
// its whole-book page here; `device`: this reader's id.
enum class Action : uint8_t {
  Nothing,
  Publish,  // this reader's place, rev = newRev()
  Ask,      // another reader went elsewhere: ask when the book is open (BookState::ask)
  Seen,     // another reader is where this one is: remember its rev
};
Action decide(const BookState &s, uint32_t local, uint32_t wholePages, const Record *remote,
              uint32_t mapped, const char *device);
uint32_t newRev(const BookState &s, const Record *remote);

// After a sync or an answer: the broker's rev and this reader's place now.
void markSynced(BookState &s, uint32_t rev, uint32_t page, uint32_t pages);
void markAsk(BookState &s, const Record &remote);
// "Stay": this reader's place wins (published next time).
void markStay(BookState &s);
// The other reader's place is in another part than the one on this reader.
void markElsewhere(BookState &s, uint32_t page, uint32_t pages);
// A new copy of the book arrived: all of it, or a part. A place that was elsewhere is
// asked about again.
void setWhole(BookState &s);
void setPart(BookState &s, uint32_t first, uint32_t wholePages,
             const std::vector<uint32_t> &starts);

// Automatic syncs after a failed join (away from home): the next 1, 2, 4, 8, then 16
// are left out. Kept in RTC memory: no default member values (see app.cpp RtcState).
struct Backoff {
  uint8_t failures;
  uint8_t skip;
};
bool shouldTry(Backoff &b);  // false (and one fewer to skip) while skipping
void failed(Backoff &b);
void succeeded(Backoff &b);

}  // namespace possync
