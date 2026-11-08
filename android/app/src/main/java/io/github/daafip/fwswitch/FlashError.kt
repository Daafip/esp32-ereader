package io.github.daafip.fwswitch

/** Why flashing (or reading a firmware file) failed. The UI turns each into a message. */
enum class Reason {
    /** The file isn't a full ESP32-C6 image this app can write. */
    BAD_IMAGE,

    /** The board never answered in download mode. */
    NO_BOOT_MODE,

    /** USB permission refused, or the port couldn't be opened. */
    USB,

    /** The board is another chip than the image (or one this app doesn't flash). */
    WRONG_CHIP,

    /** The ROM loader answered with an error status. */
    ROM,

    /** No answer in time. */
    TIMEOUT,

    /** What the board read back differs from what was sent. */
    VERIFY,

    /** The user stopped it. */
    CANCELLED,
}

class FlashError(val reason: Reason, val detail: String = "") : Exception("$reason: $detail")
