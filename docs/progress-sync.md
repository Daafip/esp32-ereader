# Reading position sync (MQTT)

Readers that use the same MQTT broker know where the others are in a book. Read on the
T5-2.13 in bed, pick up the 7.5" in the morning: it asks "Read on elsewhere: 43% on
Keuken. Go to page 212?". NEXT goes there, MENU stays where it is.

```
reader A ──┐                                   ┌── reader B
 (sleeps:  │  t5reader/progress/<book key>     │  (wakes: reads the records,
  publish) └──►  Mosquitto (Home Assistant) ◄──┘   asks before it jumps)
                 one retained record per book
```

The broker keeps one retained record per book. It is the only shared state: there is
no server of ours. The format is our own, not KOReader's: KOReader identifies a book by
the MD5 of its EPUB and stores a place in the EPUB's structure, and the reader has
neither.

## Setting it up

1. **Mosquitto** in Home Assistant (Settings → Add-ons → *Mosquitto broker*), with a
   user for the readers: Settings → People → Users → add a user (e.g. `t5reader`), or
   the add-on's *logins* option. Any MQTT 3.1.1 broker on the home network works.
2. On each reader's page (Menu → Wi-Fi transfer, ⚙ Settings):
   - **Home Wi-Fi**, if not set yet;
   - **Reading position sync**: the broker (`homeassistant.local`, `192.168.1.10`,
     `host:1883`), the user and the password;
   - a **Name** for the reader (shown in the question on the other readers; without
     one they see the board's name, like `t5-213`);
   - **On the reader → Sync position**: *with Wi-Fi* or *automatic* (below).

   Developers can put `#define MQTT_BROKER "192.168.1.10"`, `MQTT_USER` and
   `MQTT_PASSWORD` in `firmware/include/secrets.h` instead; the page's come first.
3. The same book on both readers: from the book server, or sent from the page, from the
   same EPUB (the file name decides which book it is: see "Book key" below).

The broker's password stays on the reader; `/api/info` has the broker and the user only.
Only the settings backup (⚙ Settings → Backup) has the password, as for the home Wi-Fi.

## When it syncs

*Sync position* (Settings, on the reader and on the page):

| Setting | When |
|---|---|
| **off** (default) | never |
| **with Wi-Fi** | when the Wi-Fi is on anyway: Menu → Book server, Menu → Wi-Fi transfer on the home network; and Menu → **Sync now** |
| **automatic** | the same, and when a reading session ends (the sleep screen comes: the idle time, *Sleep*, the hold gesture) and starts (the first press after the sleep screen, switching on) |

A sync covers every book on the reader: it reads the records of all of them and
publishes those this reader moved on in. A place another reader went to is kept on the
card and asked about when that book is open (straight away, or when you open it).

The automatic sync doesn't make the page slower. At the end of a session the Wi-Fi joins
while the sleep screen is drawn, and the exchange follows: a few seconds more awake (not
measured yet: `docs/checkpoints.md`). At the
start the page comes first; the reader joins while it refreshes and asks afterwards, if
there is something to ask. A press during the sync isn't lost: it turns the page (or wakes
the reader) once the sync is done. Only a question replaces a press made before it.

Away from home the join fails after 8 s. The next 1, 2, 4, 8 and then 16 automatic syncs
are skipped, so a reader on the train doesn't search for the home network at every wake.
Menu → Sync now always tries.

## The question

"Read on elsewhere / 43% on Keuken / Go to page 212?" The percentage is where the other
reader is, the page is that place on this reader. **NEXT** (go) jumps there.
**MENU** (stay) keeps this page, and this reader's place goes to the broker at the next
sync, so the other reader is asked in turn.

Nothing is asked when this reader is already on that page. A second question about the
same record doesn't come: until it is answered, the book isn't published from this
reader either.

**Parts** (a FireBeetle 7.5" without a card reads a big book in parts): when the place is
in another part, the reader says so ("in another part: page 2876", the whole book's page)
and any key goes back. Download that part from the book server: when it opens, the
question comes again, now with its page.

## How a page on one screen is found on another

Page numbers mean nothing between a T5-2.13 (250×122) and a 7.5" (480×800). A record
says where the page starts as:

- the **contents entry** it is in and how far into it, in millionths; used when the other
  reader's copy has the same number of contents entries;
- otherwise **how far into the whole book**, in millionths.

Measured on three of David's books (2026-11-15, every page of one screen mapped onto the
other and compared with the page that really holds that text):

| From → to | By chapter: same page / within one / average off | By the whole book: average off |
|---|---|---|
| T5-2.13 → 7.5" | 74-81% / 100% / 0.2-0.25 pages | 0.8-2 pages |
| 7.5" → T5-2.13 | 23-32% / 53-61% / 1.4-1.8 pages (up to 7 early, 3 late) | 5.7-14 pages (up to 22) |

A small page holds a seventh of a big one, so going from the 7.5" to the T5-2.13 can't do
better than "around here": mostly a few pages early, now and then one to three late. An
exact place would need a text position per page in the `.t5b` (not done).

A part doesn't know the whole book by itself. The book server sends the whole book's
contents with the parts (`toc` records, add-on 2.3.0 and later) and the page sends them
with "Send a part" (`POST /api/part`). The reader keeps them in the book's sync state. A
part from an older add-on syncs by the fraction of the book; a part made by
`bookprep --first-page`, or downloaded by firmware before 2.3.0, isn't synced (its title
says it is a part, but not where the part lies): download it again.

## Book key

`t5reader/progress/<key>`: `<key>` is FNV-1a (32 bits) over the book's file stem,
lower-cased, as 8 hex digits. The page, `bookprep` and the book server all name a book
after its source file (`De_meeste_mensen_deugen_-_Rutger_Bregman.t5b`), so the same EPUB
has the same key on every reader and every screen; a part has the whole book's name.
Renaming a book's file on one reader makes it another book for the sync.

## The record

Retained, QoS 1, plain UTF-8 text like the book server's replies (the reader has no JSON
parser): the line `T5SYNC<TAB>1`, then

```
position <rev> <ppm> <chapter> <chapterPpm> <toc> <device> <name> <title>
```

(tab-separated): `rev` goes up by one with every new place, whichever reader writes it;
`ppm` is where the page starts in millionths of the whole book; `chapter` the contents
entry (-1: before the first) and `chapterPpm` the place in it; `toc` the number of
contents entries (0: unknown); `device` 8 hex digits from the reader's MAC; `name` the
reader's name; `title` the book's (for people looking at the broker). Unknown records
are skipped, so more can be added.

Which record is newer is decided by `rev`, not by the time: the reader's clock comes from
the phone and may not be set. Each reader keeps, per book, the rev it last saw and its
own place then. A higher rev on the broker means another reader went on; the same rev and
another place here means this reader went on (it publishes rev + 1); a lower rev means
the broker lost a record (published again).

A session: CONNECT (clean session, client id `t5reader-<device>`), SUBSCRIBE to
`t5reader/progress/+` and `t5reader/sync/<device>`, PUBLISH to the second one, read until
it comes back (Mosquitto sends a client's messages in order: every retained record has
come by then; tried with Mosquitto 2.0.20 and 1,000 records), PUBLISH the records that
changed (QoS 1, retained), wait for their PUBACKs, DISCONNECT. Plain TCP on port 1883,
no TLS: the home network only, as the book server.

## On the card

`/books/.sync/<stem>.txt`, one per book, removed with it. Lines: `synced rev page pages`
(the last sync: the broker's rev, this reader's whole-book page and the whole book's page
count), `dirty` (answered "stay"), `part first wholePages` and `toc ...` (a part: its
first page, 0-based, the whole book's pages and contents), `remote ask|elsewhere|- ...`
(the last record from another reader, as above).

## Code

- `firmware/lib/core/src/position_sync.*`: book key, broker address, the place on another
  screen, the record and the state file, what to do with a book in a sync, the back-off.
  Unity tests in `firmware/test/test_core` (`pixi run fw-test`).
- `firmware/lib/core/src/mqtt_packet.*`: the MQTT 3.1.1 packets (tested against the
  examples in the MQTT text).
- `firmware/src/syncer.*`: the session (Wi-Fi, TCP, MQTT) and the files.
- `firmware/src/app.cpp`: when (`enterSleep()`, `setup()`, `pageShown()`, the book
  server, Wi-Fi transfer, Menu → Sync now), the question (`askSynced()`).
- The page: ⚙ Settings → Reading position sync (`POST`/`DELETE`/`GET /api/mqtt`), the
  backup, "Send a part" (`POST /api/part`); `transfer.cpp`, mirrored in
  `bookprep/devserver.py`.
- The book server: `toc` records with the parts (`bookprep/bookserver.py`,
  `src/bookserver.cpp`).

## Checks on the board

See `docs/checkpoints.md` → "Position sync".
