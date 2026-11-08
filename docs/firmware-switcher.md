# Firmware switcher (Android)

An Android app (`android/`) that puts either the e-reader or ESPHome on the
FireBeetle 2 ESP32-C6 + 7.5" over a USB-C OTG cable. Every switch writes the chosen
firmware again (no Wi-Fi, no second app slot needed), so it works with the flash
build (`reader_firebeetle75_flash`), which has one app slot.

**Status:** builds, and the flashing logic passes its tests against a fake ROM loader
(`pixi run android-test`). The checks on a real board are at the end.

## Install

Every release from 2.0.1 has the app: `fw-switch-<version>.apk` on the
[releases page](https://github.com/Daafip/esp32-ereader/releases). Open it on the
phone and allow installing apps from that source (the browser or the file manager). It
runs on Android 10 and later, on a phone with USB host (OTG) support.

Updating: a release signed with the project's release key installs over the copy you
have. One signed with a debug key (the release notes say which) doesn't: uninstall the
old app first, and choose the two firmware files again.

**Through F-Droid** (or Droid-ify, Neo Store): releases signed with the release key are
also in the project's own F-Droid repository on GitHub Pages. Add
`https://daafip.github.io/esp32-ereader/fdroid/repo` as a repository (F-Droid: Settings
-> Repositories -> +); the release notes give the link with the key's fingerprint, and
<https://daafip.github.io/esp32-ereader/fdroid/repo/> shows it as a QR code. F-Droid
then offers each new release as an update.

## Build

```
pixi run android-build      # -> out/android/fw-switch.apk (debug)
pixi run android-test       # unit tests on the PC
pixi run -e android bash tools/android.sh assembleRelease -PappVersion=2.0.0
                            # -> out/android/fw-switch-release.apk
```

It needs the Android SDK (platform 36) in `$ANDROID_HOME` or `~/Android/Sdk`. The JDK
and Gradle come from pixi's `android` environment. To install a build, either run
`adb install out/android/fw-switch.apk` with USB debugging on, or copy it to the phone
and open it. The app's version is the firmware version passed as `-PappVersion`
(versionCode: 2.0.0 -> 20000); local builds are 0.0.0.

## Release signing and the F-Droid repository (maintainers)

`.github/workflows/release.yml` builds `fw-switch-<version>.apk` for every release. With
the secrets below it signs it with the release key, so each release installs over the
last, and publishes the F-Droid repository on GitHub Pages (`fdroid/repo`, made by
`tools/make_fdroid_repo.sh` with fdroidserver; its index is signed with a second key).
Without them it signs the APK with the runner's debug key (a different one every time),
warns, and leaves the F-Droid repository out: F-Droid can't update an app whose
signature changes. `installer.yml` (Pages without a firmware release) makes the
repository again from the latest release's APK, as Pages is replaced as a whole.

One keystore holds both keys: `fwswitch` signs the APK, `fdroid` the repository index.
Make it once, outside the repository, and keep a backup: without it no later release
installs over the app, and F-Droid users would have to add the repository again.

```
pixi run -e android keytool -genkeypair -v -keystore ~/fwswitch.jks -alias fwswitch \
    -keyalg RSA -keysize 4096 -validity 10000
pixi run -e android keytool -genkeypair -v -keystore ~/fwswitch.jks -alias fdroid \
    -keyalg RSA -keysize 4096 -validity 10000
base64 -w0 ~/fwswitch.jks > ~/fwswitch.jks.b64   # the first secret's value
```

The keystore is PKCS12, so each key's password is the keystore's. In GitHub: Settings ->
Secrets and variables -> Actions -> New repository secret:

| Secret | Value |
|---|---|
| `FWSWITCH_KEYSTORE_B64` | the contents of `fwswitch.jks.b64` |
| `FWSWITCH_KEYSTORE_PASSWORD` | the keystore's password |
| `FWSWITCH_KEY_ALIAS` | `fwswitch` |
| `FWSWITCH_KEY_PASSWORD` | the keystore's password again |

A different alias for the repository key: the repository variable `FDROID_KEY_ALIAS`.
The next release then has a signed APK, the F-Droid repository, and the repository
link with its fingerprint in the release notes. A local release build uses the key when
`FWSWITCH_KEYSTORE` (the file) and the other `FWSWITCH_*` values are in the
environment; `tools/make_fdroid_repo.sh <apk> <dir>` makes the repository locally
(`pip install fdroidserver`).

## The two files

The app keeps one file per button. Choose each once, and the app keeps its own copy.

| Button | File |
|---|---|
| E-reader | `out/firmware/firebeetle-75-flash-reader-<version>.bin` from `pixi run fw-dist` |
| ESPHome | the factory image: ESPHome Builder → Install → Manual download → *Factory format*, or `.esphome/build/<name>/.pioenvs/<name>/firmware.factory.bin` after `esphome compile` |

Both must be **full images** (flashed at 0x0: bootloader, partition table, app). The
app refuses app-only files (`-ota.bin`, `firmware.ota.bin`) and images for other chips.

## Keeping settings and books: ESPHome on the reader's partition table

When the board's partition table is the same as the image's, the app writes only the
bootloader and the app. The reader's settings (`nvs` at 0x9000) and the books in flash
(`books` at 0x1F0000) stay, and ESPHome keeps its own settings in the same `nvs`
under its own namespace. For that, build ESPHome on the reader's table: put a copy
of `firmware/partitions-fb75-flash.csv` next to the YAML and add:

```yaml
esp32:
  board: esp32-c6-devkitc-1
  flash_size: 4MB
  partitions: partitions-fb75-flash.csv
  framework:
    type: esp-idf
```

Leave out `ota:`. That table has one app slot, so updating ESPHome over Wi-Fi can't
work; you update it with the app instead. Not yet tried: whether ESPHome accepts a
table with a factory app and no `otadata`. If it doesn't, use ESPHome's own layout
(below).

With ESPHome's own layout, the board's table differs from the image's on every switch.
The app then asks first, writes the whole image and erases every data partition of the
new layout that the image doesn't cover: going to ESPHome its `nvs`, going back to the
reader its `books` and `coredump` (2 MB: about half a minute). The settings of the
firmware you install start fresh, and the reader formats an empty `books` partition.

The erase matters because the layouts overlap: ESPHome keeps its `nvs` (0x390000) and
the space for its second app (0x1D0000) inside the reader's `books` (0x1F0000-0x3F0000).
Until 2026-11-08 the app only blanked `nvs` partitions; back on the reader, the start of
the old file system had survived, the reader mounted it, and a book in it was damaged
("Read error", "file too short").

## Switching

1. Connect the board to the phone (USB-C OTG cable or adapter).
2. Tap **Install e-reader** or **Install ESPHome**.
3. If the board is awake, the app restarts it into download mode itself. If it
   sleeps, its USB port is off: the app says so and waits 90 s. Hold **BOOT**, press
   **RST**, release **BOOT**.
4. The app writes what differs (parts the board already has are skipped), reads it
   back (MD5), and restarts the board into the new firmware.

Android asks for USB permission. Tick "always open with this app" in the dialog that
appears when the board is plugged in, or you will be asked again each time the board
re-appears on USB.

## How it works

It works as `esptool --no-stub write_flash 0x0 image.bin` does, ported from esptool's
source (`loader.py`, `targets/esp32c3.py` and `esp32c6.py`, esp-pylib's
`serial_reset.py`):

- **USB:** Espressif's USB-Serial-JTAG (303A:1001) is a CDC-ACM device. DTR and RTS go
  out as `SET_CONTROL_LINE_STATE`. Entering download mode uses esptool's
  USB-Serial-JTAG sequence; restarting holds RTS for 0.2 s.
- **ROM loader:** SLIP frames; `SYNC`; `GET_SECURITY_INFO` checks for an ESP32-C6
  (chip id 13), and refuses secure download mode and flash encryption. The RTC
  watchdog is switched off and the super watchdog set to feed itself (esptool does
  this over USB-Serial-JTAG, or the chip can reset in the middle of a write). Then
  `SPI_ATTACH` and `SPI_SET_PARAMS` (flash size from the image header).
- **Plan:** MD5 of the board's partition table (0x8000, 0xC00 bytes) against the
  image's, or against the other file's when its entries are the same. If they match,
  it writes the image minus its data partitions (`otadata` is still written).
  Otherwise it writes the whole image, and blanks the data partitions of the new
  layout (everything but apps) where they lie beyond the end of the image.
- **Writing:** zlib level 9, 1 KB blocks (`FLASH_DEFL_BEGIN`/`DATA`; the ROM erases on
  BEGIN), a timeout per block from the bytes it inflates to, and an MD5 read-back
  per stretch. No `FLASH_DEFL_END`, because to the ROM that means "leave the loader".

Code: `FirmwareImage.kt` (image, table, plan), `EspLoader.kt` (protocol), `Flasher.kt`
(the run), `UsbCdcPort.kt` (Android USB), `MainActivity.kt` (screen). Only the last two
use Android APIs; the tests (`app/src/test`) run the rest against `FakeRom`.

## Limits

- ESP32-C6 boards on their own USB port only. USB-UART bridges (CH340, CP210x) and
  other chips are refused.
- No "latest release from GitHub" button yet. The flash build's image is in the
  releases from 2.0.0 on (`firebeetle-75-flash-reader-<version>.bin`): download it and
  choose it in the app.

## Checks on the real board (🧑)

- [ ] The phone sees the board (303A:1001) and the permission dialog works
- [ ] Download mode: reset over USB while the reader is awake; BOOT + RST while it sleeps
- [ ] Reader → ESPHome on the reader's table: settings and books still there after switching back
- [ ] ESPHome with `partitions: partitions-fb75-flash.csv` builds and runs (factory app, no otadata)
- [ ] ESPHome's own layout: the app asks first, then whole-image writes both ways; back on
  the reader the book space is empty (formatted at start-up), no damaged books
- [ ] How long a switch takes
