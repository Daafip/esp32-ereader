# Esp32 E-reader

> [!WARNING]  
> Although tested by myself, this project was fully coded by AI without human review of the code itself, use at your own risk.

Inspired by [Paul Lagier](https://www.youtube.com/watch?v=IL05zoHBGwA), but his [code](https://github.com/PaulLagier/pala-one-firmware) was only for the `Heltec V1 e-ink` board, so i took the idea and expanded it to other models with my own twist.

## Supported models

**Install from the browser:** <https://daafip.github.io/esp32-ereader/> (Chrome or Edge,
reader on USB; leave "Erase everything" unticked to keep your settings).

Firmware available at the [releases](https://github.com/Daafip/esp32-ereader/releases) or build yourself `pixi run fw-build`.
Flash with platform io, esptools or `pixi run fw-flash`

| Board | Flash over USB (at `0x0`) | Update over Wi-Fi | Tested? |
|---|---|---|---|
| LilyGo T5-2.13 | `t5-reader-x.x.x.bin` | `t5-reader-x.x.x-ota.bin` | Yes |
| LilyGo T5-4.7" S3 | `t5-47-reader-x.x.x.bin` | `t5-47-reader-x.x.x-ota.bin` | NO - let me know! |
| M5Stack M5PaperS3 | `m5papers3-reader-x.x.x.bin` | `m5papers3-reader-x.x.x-ota.bin` | NO - let me know! | 
| M5Stack M5Paper | `m5paper-reader-x.x.x.bin` | `m5paper-reader-x.x.x-ota.bin` | NO - let me know! | 

The 4.7" boards and the M5 boards have not run this firmware yet: see
[docs/checkpoints.md](docs/checkpoints.md) for what to check.

## How it works

Books are typeset ahead of time into 1-bit page images (`.t5b`), so the reader only
copies pages to the screen. Drop an EPUB or TXT on the reader's Wi-Fi page and your
phone does the typesetting (font, size, margins, hyphenation), or convert on a PC with
`pixi run prep book.epub`. More in [docs/usage.md](docs/usage.md); hardware notes in
[docs/hardware.md](docs/hardware.md), [docs/hardware-t5-47.md](docs/hardware-t5-47.md)
and [docs/hardware-m5paper.md](docs/hardware-m5paper.md).

Also on board: chapters, bookmarks, search (on the phone, jumping the reader to a hit),
library folders, reading statistics, a lock, sleep screens (book, cover, your own
pictures, a square picture beside the book, or the page itself), a status bar that can shrink or hide, OpenDyslexic and
bionic reading, English and Dutch menus, and settings, home Wi-Fi and factory reset
from the phone.

## References

Hardware and display drivers:

- [bb_epaper](https://github.com/bitbank2/bb_epaper) (GPL-3.0) by Larry Bank: SPI e-paper driver and the drawing/font code for every board
- [FastEPD](https://github.com/bitbank2/FastEPD) (Apache-2.0) by Larry Bank: parallel e-paper driver for the T5-4.7" and the M5PaperS3
- [LilyGo T5 e-paper series](https://github.com/Xinyuan-LilyGO/LilyGo-T5-Epaper-Series) and [LilyGo-EPD47](https://github.com/Xinyuan-LilyGO/LilyGo-EPD47): pinouts and schematics of the LilyGo boards
- [M5Unified](https://github.com/m5stack/M5Unified) and [M5GFX](https://github.com/m5stack/M5GFX): pinouts and power handling of the M5 boards, and the IT8951 protocol used for the M5Paper

Firmware build and flashing:

- [pioarduino](https://github.com/pioarduino/platform-espressif32): PlatformIO platform for Arduino-ESP32 3.x
- [esptool-js](https://github.com/espressif/esptool-js) (Apache-2.0) by Espressif: the browser installer
- [esptool-js](https://espressif.github.io/esptool-js/): flashing from the browser
- [QR Code generator](https://github.com/nayuki/QR-Code-generator) (MIT) by Project Nayuki: the Wi-Fi QR codes

Book conversion:

- [Pillow](https://python-pillow.org), [EbookLib](https://github.com/aerkalov/ebooklib), [fontTools](https://github.com/fonttools/fonttools)
- [Pyphen](https://github.com/Kozea/Pyphen): hyphenation patterns (also exported for the in-browser converter)
- Fonts: [Ubuntu](https://design.ubuntu.com/font) (default), and Literata, Atkinson Hyperlegible and Merriweather from [Google Fonts](https://github.com/google/fonts) (SIL OFL), [OpenDyslexic](https://github.com/antijingoist/opendyslexic) by Abbie Gonzalez (SIL OFL)

Tooling: [pixi](https://pixi.sh) runs everything (`pixi run --list`).

## License

GPL-3.0-or-later, see [LICENSE](LICENSE). The firmware links bb_epaper (GPL-3.0-or-later),
so the firmware images are under the GPL as well.

Third-party parts keep their own licences:

- [FastEPD](https://github.com/bitbank2/FastEPD): Apache-2.0 (downloaded at build time;
  `firmware/src/fastepd_panel.cpp` contains its waveform table)
- [QR Code generator](https://github.com/nayuki/QR-Code-generator), vendored in
  `firmware/lib/qrcodegen`: MIT
- Fonts: Ubuntu Font Licence 1.0 (Ubuntu; also the bitmap UI fonts in
  `firmware/include/fonts`) and SIL OFL 1.1 (Literata, Atkinson Hyperlegible,
  Merriweather, OpenDyslexic; OpenDyslexic is shipped unmodified, as its Reserved
  Font Name requires)
- Hyphenation patterns from Pyphen: their own licences

The reader serves the full font licences and the pattern notices at `/licenses.txt`
on its Wi-Fi page.
