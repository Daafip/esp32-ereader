# T5 book server

Your EPUB library for the T5 e-reader. The add-on reads the books in a folder,
typesets them on Home Assistant for the reader that asks (a profile: screen, font,
size, ...), and the reader downloads them itself over the home Wi-Fi: **Menu → Book
server**. No phone needed, except once to enter the address.

## Setting up

1. **Configuration**: set `library` to the folder with your books (default
   `/share/calibre-dedrm/books`, where the Calibre DeDRM add-on puts them), and add a
   profile per reader (below). Start the add-on.
2. The **log** shows the address for the reader, like
   `Address for the reader (its page: Settings, Book server): http://192.168.68.101:8484`.
   **Open web UI** shows it too, with your books.
3. On the reader: Menu → Wi-Fi transfer, open its page on your phone, ⚙ Settings.
   Enter your **Home Wi-Fi** (if you haven't yet) and the **Book server** address. Done.
4. From then on: Menu → **Book server** on the reader.

## On the reader

Menu → Book server joins the home Wi-Fi and shows, after a profile if there are
several for its screen: **Newest**, **Authors**, **Titles A–Z**. Pick a book: the
add-on typesets it (the reader shows how far; a minute or two on a Raspberry Pi, then
it is kept), and the reader downloads it. **read** opens it, **more** goes back to the
list. A dot marks books that are on the reader already; downloading one again (with
another profile, say) replaces it, the reading position moving along.

A book bigger than the room on the reader (a FireBeetle 7.5" without a card has about
2 MB) comes in **parts** that fit: "Pages 1–151", "Pages 152–297", ... Each part
replaces the last, and the list starts on the next one. The part's title has its
pages in it, so the library tells them apart.

## Configuration

```yaml
library: /share/calibre-dedrm/books
profiles:
  - name: Literata 18
    screen: 7.5in
    font: Literata
    size: 18
  - name: Large
    screen: 7.5in
    size: 22
    line_spacing: 1.4
  - name: Landscape
    screen: 7.5in-landscape
    font: Atkinson Hyperlegible
  - name: Pocket
    screen: t5-213
prepare_newest: 5
cache_mb: 2000
```

**library**: every `.epub` and `.txt` in this folder and its subfolders. Title,
authors and "added" come from Calibre's `.opf` next to a book (or `metadata.opf` in a
Calibre library's book folder), else from the EPUB itself. A Calibre library *inside*
the folder (a subfolder with a `metadata.db`) is skipped, because its books are
Calibre's copies of ones already in the folder. To serve a Calibre library, name its
own folder here. `/share` and `/media` can be read (not written).

**profiles**: what the reader can choose from. It lists the profiles for its own
screen; a screen without any gets a default one (Ubuntu at the screen's usual size).

| Option | Meaning | Default |
|---|---|---|
| `name` | shown on the reader | (required) |
| `screen` | `t5-213` (LilyGo T5-2.13), `4.7in` (T5-4.7" S3, M5PaperS3, M5Paper), `7.5in` (FireBeetle 7.5" portrait), `7.5in-landscape` | (required) |
| `font` | Ubuntu, Literata, Atkinson Hyperlegible, Merriweather, OpenDyslexic, or a `.ttf`/`.otf` you put in `/share/t5books/fonts` (by file name) | Ubuntu |
| `size` | text size in pixels | the screen's: 13, 28, 18, 22 |
| `line_spacing` | line height as a multiple of the size | 1.2 |
| `margin` | left, right and top margin in pixels | the screen's |
| `indent` | first-line indent in pixels | the size |
| `paragraph_spacing` | extra pixels between paragraphs | 0 |
| `justify` | justified, or ragged right | true |
| `hyphenate` | hyphenation | true |
| `language` | hyphenation language (`nl`, `en`, `de`, ...) | the book's |
| `weight` | 70 (lighter) to 210 (bolder) | 128 |
| `bionic` | bolder word starts | false |
| `cover` | keep the cover (sleep screen) | true |
| `chapter_break` | each chapter on a new page | true |

These are the reader page's converter options and `bookprep`'s (`pixi run prep book.epub
--help` in the repository). Changing a profile typesets its books again.

**prepare_newest**: this many of the newest books are typeset ahead of time for every
profile, so they download at once. 0: only when a reader asks.

**cache_mb**: typeset books are kept in the add-on's data (not in backups) until the
cache is this big; the least recently used go first.

## Notes

- Like COPS, the book server has no login: anyone on your home network can list and
  download the books. Don't forward its port to the internet.
- The reader speaks plain HTTP. Its address is Home Assistant's IP with the port
  (8484 unless you change it under Network). A name like `homeassistant.local`
  should work too (the reader asks over mDNS); the IP address is the safe choice.
- More: `docs/book-server.md` in the repository.
