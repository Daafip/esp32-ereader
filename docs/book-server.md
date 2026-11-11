# Book server (Home Assistant add-on)

The reader downloads books by itself, over the home Wi-Fi, from a server that holds
your EPUB library and typesets each book for the reader that asks. The server is a
Home Assistant add-on (`homeassistant/t5books/`), or `pixi run book-server` on any PC.
Like COPS for Calibre, but the result is a `.t5b` for this reader instead of an EPUB.

```
/share/.../books/*.epub ──► book server ──(profile: screen, font, size, ...)──► bookprep
   (Calibre .opf beside      (add-on, port 8484)                              │
    each book: metadata)            ▲            cache: /data/cache/<profile>/<book>.t5b
                                    │ HTTP, plain text
                     reader: Menu → Book server (home Wi-Fi)
```

What is typeset how is decided in the add-on's configuration (**profiles**); on the
reader you only choose a profile (when its screen has more than one) and a book.

## Installing the add-on

Either way, Home Assistant builds the image itself (amd64 and aarch64; a minute or so,
with internet for Python packages and the fonts).

**From this repository** (the released version): Settings → Add-ons → Add-on store →
⋮ → Repositories → add `https://github.com/Daafip/esp32-ereader`, then install
*T5 book server*. Its image takes `tools/bookprep` from the GitHub release of the
add-on's version (`v<version>`), so this works once that release exists.

**As a local add-on** (the code you have, also before a release):

```sh
pixi run ha-addon                       # -> out/ha-addon/t5books/ (with tools/bookprep)
pixi run ha-addon /mnt/ha_addons        # or straight into HA's addons folder (Samba share)
```

Copy `out/ha-addon/t5books` into Home Assistant's `/addons` folder (the Samba
share "addons") if you didn't give it directly, then Add-on store → ⋮ → Check for
updates → *Local add-ons* → *T5 book server*. After a change: run it again and press
**Rebuild** on the add-on's page. Books typeset by an older bookprep are made again
(the cache key includes a hash of bookprep's code).

Then the add-on's **Configuration** (library folder and profiles: see the add-on's
Documentation tab, `homeassistant/t5books/DOCS.md`), **Start**, and read the address
for the reader in its **Log** (`Address for the reader ...: http://192.168.68.101:8484`).

## On the reader

Once, on the reader's page (Menu → Wi-Fi transfer, ⚙ Settings): **Home Wi-Fi** and
**Book server** (that address). Developers can put `#define BOOK_SERVER "http://..."`
in `firmware/include/secrets.h` instead; the page's comes first.

Menu → **Book server** (also in the short menu without a book, and MENU on "No books
yet"):

1. joins the home Wi-Fi (the reader's own network can't reach Home Assistant);
2. lists the profiles for its screen, if there are several (a screen without one gets
   the default profile);
3. **Newest**, **Authors**, **Titles A–Z**; a dot marks a book already on the reader
   (same file name). The 7.5" in landscape shows the author beside the title;
4. a book: **Typesetting** with a progress bar while the server makes it (hold MENU
   to stop waiting; the server goes on), then **Downloading**, then **Downloaded**:
   NEXT reads it, MENU goes back for more.

A book goes where one of its name already is (a folder, or `/books`), replacing it;
the reading position moves along when it is the same book (another profile). A
book bigger than the room left comes in **parts** ("Too big: choose a part", pages
and size per part): the dot marks the part on the reader and the list starts on the
next. A part replaces the previous one, which is removed first when its room is
needed; its title carries the pages, "Dieren eten (152–297)", and it has its own
position and contents. This is how the FireBeetle 7.5" without a card (2 MB) reads a
whole book.

Wi-Fi is on only in these screens; the idle timeout ends them as usual (not while
typesetting: that has its own limit of 20 minutes).

## Running it without Home Assistant

```sh
pixi run book-server --library ~/Books                          # default profiles only
pixi run book-server --options profiles.json --port 8484        # the add-on's options
```

`profiles.json` has the add-on's options (`/data/options.json`): `library`,
`profiles`, `prepare_newest`, `cache_mb`. The cache is `out/book-server/cache`, the
bundled fonts as TTF `out/book-server/fonts` (downloaded once, pinned like the page's).
Own fonts: `--fonts DIR` (the add-on: `/share/t5books/fonts`).

## Protocol

The reader has no JSON parser: replies are UTF-8 text, the line `T5BOOKS<TAB>1`, then
one record per line, fields separated by tabs (titles at most 90 bytes). HTTP/1.0,
`Content-Length` on every reply. Pages are 1-based. Errors come with a 4xx status and
an `error<TAB>message` record.

| Request | Reply records |
|---|---|
| `GET /api/profiles?screen=7.5in` | `profile id name` per profile for that screen |
| `GET /api/books?sort=new\|title&offset=&count=`, or `?author=<id>` | `total n`, then `book id title author file` |
| `GET /api/authors?offset=&count=` | `total n`, then `author id name books` |
| `GET /api/prepare?profile=&book=&room=<bytes>` | `busy percent ahead`, `error message`, or `ready pages bytes file` plus, when bytes > room, `part first last bytes` per part that fits (none: no room for a page) |
| `GET /api/download?profile=&book=[&first=&last=]` | the `.t5b` (409 when not typeset yet) |

Plus `/` (a page for browsers: books, profiles, download links) and `/health`.

Code: `tools/bookprep/src/bookprep/bookserver.py` (server, profiles, conversion queue,
cache) and `library.py` (scanning, metadata); parts are cut from the cached book
without typesetting again (`t5b.write_part`: the stored page streams, its own contents,
`part_title`). Reader: `firmware/src/bookserver.cpp` (HTTP), `lib/core/src/catalog.*`
(address and reply parsing, tested), the screens in `app.cpp`. The reader's page sets
the address through `POST /api/bookserver` (`transfer.cpp`, mirrored in `devserver.py`).

## Checks on the board

- [ ] Set Home Wi-Fi and the book server address on the page; Menu → Book server
      lists the profiles / Newest
- [ ] Download a small book on the T5-2.13 (SD): progress, "Downloaded", read it
- [ ] FireBeetle 7.5" without a card: a big book offers parts; part 2 replaces part 1,
      the list starts on part 2 next time
- [ ] Wrong address / add-on stopped: "Book server unreachable", any key back
- [ ] Idle in the lists: the reader sleeps, Wi-Fi off
