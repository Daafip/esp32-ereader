# Esp32 E-reader

> [!WARNING]  
> Although tested by myself, this project was fully coded by AI without human review of the code itself, use at your own risk.

Inspired by [Paul Lagier](https://www.youtube.com/watch?v=IL05zoHBGwA), but his design costs 5 euros - why pay when you can spend endless hours instructing claude code?

## Supported models

Firmware available at the [releases](https://github.com/Daafip/esp32-ereader/releases) or build yourself `pixi run fw-build`.
Flash with platform io, esptools or `pixi run fw-flash`

| Board | Flash over USB (at `0x0`) | Update over Wi-Fi |
|---|---|---|
| LilyGo T5-2.13 | `t5-reader-x.x.x.bin` | `t5-reader-x.x.x-ota.bin` |
| LilyGo T5-4.7" S3 | `t5-47-reader-x.x.x.bin` | `t5-47-reader-x.x.x-ota.bin` |
| M5Stack M5PaperS3 | `m5papers3-reader-x.x.x.bin` | `m5papers3-reader-x.x.x-ota.bin` |
| M5Stack M5Paper | `m5paper-reader-x.x.x.bin` | `m5paper-reader-x.x.x-ota.bin` |

The 4.7" boards and the M5 boards have not run this firmware yet: see
[docs/checkpoints.md](docs/checkpoints.md) for what to check.

## How it works

Books are typeset ahead of time into 1-bit page images (`.t5b`), so the reader only
copies pages to the screen. Drop an EPUB or TXT on the reader's Wi-Fi page and your
phone does the typesetting (font, size, margins, hyphenation), or convert on a PC with
`pixi run prep book.epub`. More in [docs/usage.md](docs/usage.md); hardware notes in
[docs/hardware.md](docs/hardware.md), [docs/hardware-t5-47.md](docs/hardware-t5-47.md)
and [docs/hardware-m5paper.md](docs/hardware-m5paper.md).

## References

Hardware and display drivers:

- [bb_epaper](https://github.com/bitbank2/bb_epaper) (GPL-3.0) by Larry Bank: SPI e-paper driver and the drawing/font code for every board
- [FastEPD](https://github.com/bitbank2/FastEPD) (Apache-2.0) by Larry Bank: parallel e-paper driver for the T5-4.7" and the M5PaperS3
- [LilyGo T5 e-paper series](https://github.com/Xinyuan-LilyGO/LilyGo-T5-Epaper-Series) and [LilyGo-EPD47](https://github.com/Xinyuan-LilyGO/LilyGo-EPD47): pinouts and schematics of the LilyGo boards
- [M5Unified](https://github.com/m5stack/M5Unified) and [M5GFX](https://github.com/m5stack/M5GFX): pinouts and power handling of the M5 boards, and the IT8951 protocol used for the M5Paper

Firmware build and flashing:

- [pioarduino](https://github.com/pioarduino/platform-espressif32): PlatformIO platform for Arduino-ESP32 3.x
- [esptool-js](https://espressif.github.io/esptool-js/): flashing from the browser
- [QR Code generator](https://github.com/nayuki/QR-Code-generator) (MIT) by Project Nayuki: the Wi-Fi QR codes

Book conversion:

- [Pillow](https://python-pillow.org), [EbookLib](https://github.com/aerkalov/ebooklib), [fontTools](https://github.com/fonttools/fonttools)
- [Pyphen](https://github.com/Kozea/Pyphen): hyphenation patterns (also exported for the in-browser converter)
- Fonts: [Ubuntu](https://design.ubuntu.com/font) (default), and Literata, Atkinson Hyperlegible and Merriweather from [Google Fonts](https://github.com/google/fonts) (SIL OFL)

Tooling: [pixi](https://pixi.sh) runs everything (`pixi run --list`).

Because bb_epaper is GPL-3.0, the firmware images are GPL-3.0 as well.
