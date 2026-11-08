package io.github.daafip.fwswitch

/** Where a flash run is, for the UI. */
enum class Step { CONNECTING, RESETTING, WAITING_FOR_BOOT_MODE, CHECKING, WRITING, VERIFYING, RESTARTING, DONE }

interface FlashListener {
    fun onStep(step: Step) {}
    fun onProgress(done: Long, total: Long) {}
    fun log(line: String) {}

    /** The board has another partition table: may the whole image go on (fresh settings)? Blocks. */
    fun confirmWholeImage(): Boolean
}

/**
 * Puts a full image on an ESP32-C6 board over its USB-Serial-JTAG port, as
 * `esptool --no-stub write_flash 0x0 image.bin` would, but keeping the board's settings
 * and books when its partition table already matches the image's (see [Planner]).
 * Stretches the board already has (same MD5) are skipped.
 */
class Flasher(
    private val opener: PortOpener,
    private val listener: FlashListener,
    private val clock: Clock = Clock.System,
) {
    /** Stops the wait for BOOT + RST (writing itself isn't interrupted). */
    @Volatile
    var cancelled = false

    private class Link(val port: SerialPort, val esp: EspLoader)

    /**
     * Writes [image] and restarts the board. [known] are the other firmware files: a board
     * table equal to theirs counts as the same layout when the entries match. Returns false
     * when the user declined a whole-image write (the board then restarts unchanged).
     */
    fun flash(image: FirmwareImage, known: List<FirmwareImage> = emptyList()): Boolean {
        val link = connect()
        try {
            val esp = link.esp
            listener.onStep(Step.CHECKING)
            val info = try {
                esp.securityInfo()
            } catch (e: FlashError) {
                // Chips before the S2/C3 generation don't have the command
                throw FlashError(Reason.WRONG_CHIP, "the board isn't an ESP32-C6 (${e.detail})")
            }
            if (info.chipId != image.chipId) {
                throw FlashError(
                    Reason.WRONG_CHIP,
                    "board: ${FirmwareImage.chipName(info.chipId)}, image: ${FirmwareImage.chipName(image.chipId)}",
                )
            }
            if (info.flags and SECURE_DOWNLOAD_ENABLE != 0) throw FlashError(Reason.ROM, "secure download mode is on")
            if (Integer.bitCount(info.flashCryptCnt and 0x7) % 2 == 1) throw FlashError(Reason.ROM, "flash encryption is on")
            esp.disableWatchdogs()
            esp.spiAttach()
            esp.spiSetParams(image.flashSize)

            val boardTable = esp.flashMd5(PartitionTable.OFFSET, PartitionTable.LENGTH)
            val keep = Planner.sameLayout(boardTable, image, known)
            if (keep) {
                listener.log("partition table matches: settings and data stay")
            } else {
                listener.log("partition table differs ($boardTable)")
                if (!listener.confirmWholeImage()) {
                    listener.onStep(Step.RESTARTING)
                    esp.hardReset()
                    return false
                }
            }

            val regions = if (keep) Planner.keepData(image) else Planner.wholeImage(image)
            val total = regions.sumOf { it.length.toLong() }
            var done = 0L
            listener.onProgress(0, total)
            for (r in regions) {
                val data = Planner.data(image, r)
                val md5 = FirmwareImage.md5Hex(data)
                if (esp.flashMd5(r.offset, r.length) == md5) {
                    listener.log("0x%06x: %d bytes already there".format(r.offset, r.length))
                } else {
                    listener.onStep(Step.WRITING)
                    listener.log("0x%06x: writing %d bytes%s".format(r.offset, r.length, if (r.blank) " (blank)" else ""))
                    esp.writeFlash(r.offset, data) { sent, count ->
                        listener.onProgress(done + r.length.toLong() * sent / count, total)
                    }
                    listener.onStep(Step.VERIFYING)
                    val got = esp.flashMd5(r.offset, r.length)
                    if (got != md5) {
                        throw FlashError(Reason.VERIFY, "0x%06x: the board has %s, sent %s".format(r.offset, got, md5))
                    }
                }
                done += r.length
                listener.onProgress(done, total)
            }

            listener.onStep(Step.RESTARTING)
            esp.hardReset()
            listener.onStep(Step.DONE)
            return true
        } finally {
            closeQuietly(link.port)
        }
    }

    private fun tryLink(port: SerialPort): Link? {
        val esp = EspLoader(port, clock)
        return if (esp.connect()) Link(port, esp) else null
    }

    /**
     * Download mode, in order: already there; the firmware runs (USB present), so reset
     * into it over USB-Serial-JTAG; else the board sleeps (USB off) or the reset didn't
     * take, and the user holds BOOT and presses RST.
     */
    private fun connect(): Link {
        listener.onStep(Step.CONNECTING)
        opener.open(FIRST_WAIT_MS)?.let { port ->
            tryLink(port)?.let { return it }
            listener.onStep(Step.RESETTING)
            listener.log("no answer: resetting into download mode")
            EspLoader(port, clock).usbJtagBootloaderReset()
            closeQuietly(port)
            clock.sleep(500)
            opener.open(REAPPEAR_WAIT_MS)?.let { again ->
                tryLink(again)?.let { return it }
                closeQuietly(again)
            }
        }
        listener.onStep(Step.WAITING_FOR_BOOT_MODE)
        listener.log("waiting for BOOT + RST")
        val deadline = clock.now() + BOOT_BUTTON_WAIT_MS
        while (clock.now() < deadline) {
            if (cancelled) throw FlashError(Reason.CANCELLED)
            val port = opener.open(1_000) ?: continue
            tryLink(port)?.let { return it }
            closeQuietly(port)
            clock.sleep(500)
        }
        throw FlashError(Reason.NO_BOOT_MODE)
    }

    private fun closeQuietly(port: SerialPort) {
        try {
            port.close()
        } catch (_: Exception) {
        }
    }

    companion object {
        private const val FIRST_WAIT_MS = 2_000L
        private const val REAPPEAR_WAIT_MS = 5_000L
        private const val BOOT_BUTTON_WAIT_MS = 90_000L
        private const val SECURE_DOWNLOAD_ENABLE = 1 shl 2
    }
}
