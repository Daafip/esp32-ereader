package io.github.daafip.fwswitch

import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.zip.Deflater
import java.util.zip.Inflater
import kotlin.math.max
import kotlin.math.min

/**
 * Talks to the ESP32-C6 ROM loader (download mode) the way esptool does without its stub:
 * SLIP frames, compressed flash writes, MD5 read-back, and the USB-Serial-JTAG reset
 * sequences. Opcodes, timeouts and sequences follow esptool (loader.py, targets/esp32c3.py
 * and esp32c6.py, esp-pylib serial_reset.py).
 */
class EspLoader(private val port: SerialPort, private val clock: Clock = Clock.System) {
    class Response(val value: Int, val body: ByteArray)

    /** What GET_SECURITY_INFO says about the chip. */
    class SecurityInfo(val flags: Int, val flashCryptCnt: Int, val chipId: Int)

    private val decoder = Slip.Decoder()
    private val frames = ArrayDeque<ByteArray>()
    private val buf = ByteArray(16384)

    // --- framing --------------------------------------------------------------------------

    private fun send(op: Int, data: ByteArray, chk: Int) {
        val pkt = ByteBuffer.allocate(8 + data.size).order(ByteOrder.LITTLE_ENDIAN)
            .put(0).put(op.toByte()).putShort(data.size.toShort()).putInt(chk).put(data)
        port.write(Slip.encode(pkt.array()))
    }

    private fun nextFrame(deadline: Long): ByteArray? {
        while (true) {
            frames.removeFirstOrNull()?.let { return it }
            val left = deadline - clock.now()
            if (left <= 0) return null
            val n = port.read(buf, min(left, 200L).toInt().coerceAtLeast(1))
            if (n > 0) frames.addAll(decoder.feed(buf, n))
        }
    }

    /** Reads frames until the answer to [op] arrives (esptool: up to 100 frames). */
    private fun receive(op: Int, timeoutMs: Long): Response {
        val deadline = clock.now() + timeoutMs
        repeat(100) {
            val f = nextFrame(deadline)
                ?: throw FlashError(Reason.TIMEOUT, "no answer to command 0x%02x".format(op))
            if (f.size < 8 || f[0].toInt() != 1) return@repeat
            val bb = ByteBuffer.wrap(f).order(ByteOrder.LITTLE_ENDIAN)
            val opRet = f[1].toInt() and 0xFF
            val value = bb.getInt(4)
            val body = f.copyOfRange(8, f.size)
            if (opRet == op) return Response(value, body)
            if (body.size >= 2 && body[0].toInt() != 0 && body[1].toInt() == ROM_INVALID_RECV_MSG) {
                throw FlashError(Reason.ROM, "command 0x%02x not supported".format(op))
            }
        }
        throw FlashError(Reason.ROM, "answer doesn't match command 0x%02x".format(op))
    }

    fun command(op: Int, data: ByteArray = ByteArray(0), chk: Int = 0, timeoutMs: Long = DEFAULT_TIMEOUT): Response {
        send(op, data, chk)
        return receive(op, timeoutMs)
    }

    /**
     * A command whose answer ends in status bytes (ROM: status, error, two reserved),
     * after [respLen] bytes of data. Returns the answer, data cut to [respLen].
     */
    fun checkCommand(
        what: String,
        op: Int,
        data: ByteArray = ByteArray(0),
        chk: Int = 0,
        respLen: Int = 0,
        timeoutMs: Long = DEFAULT_TIMEOUT,
    ): Response {
        val r = command(op, data, chk, timeoutMs)
        if (r.body.size < respLen + 2) {
            if (r.body.size >= 2 && r.body[0].toInt() != 0) romError(what, r.body[1])
            throw FlashError(Reason.ROM, "$what: answer too short (${r.body.size} bytes)")
        }
        if (r.body[respLen].toInt() != 0) romError(what, r.body[respLen + 1])
        return Response(r.value, r.body.copyOf(respLen))
    }

    private fun romError(what: String, code: Byte): Nothing {
        val c = code.toInt() and 0xFF
        val meaning = ROM_ERRORS[c] ?: "unknown error"
        throw FlashError(Reason.ROM, "$what: 0x%02x %s".format(c, meaning))
    }

    /** Drops whatever is waiting (boot log, stale answers). */
    fun flushInput() {
        for (i in 0 until 50) {
            if (port.read(buf, 10) <= 0) break
        }
        decoder.reset()
        frames.clear()
    }

    // --- connecting -----------------------------------------------------------------------

    /** One SYNC exchange; true when the ROM loader answered. */
    fun sync(): Boolean {
        val payload = byteArrayOf(0x07, 0x07, 0x12, 0x20) + ByteArray(32) { 0x55 }
        try {
            command(SYNC, payload, timeoutMs = SYNC_TIMEOUT)
        } catch (e: FlashError) {
            if (e.reason == Reason.TIMEOUT) return false
            throw e
        }
        // The ROM answers a SYNC up to eight times: take the rest so they don't linger.
        repeat(7) {
            try {
                receive(SYNC, SYNC_TIMEOUT)
            } catch (_: FlashError) {
                return true
            }
        }
        return true
    }

    /** A few SYNC tries (esptool: five per reset); true when the loader answered. */
    fun connect(): Boolean {
        repeat(5) {
            flushInput()
            if (sync()) return true
            clock.sleep(50)
        }
        return false
    }

    fun securityInfo(): SecurityInfo {
        val r = checkCommand("read chip info", GET_SECURITY_INFO, respLen = 20)
        val bb = ByteBuffer.wrap(r.body).order(ByteOrder.LITTLE_ENDIAN)
        return SecurityInfo(flags = bb.getInt(0), flashCryptCnt = r.body[4].toInt() and 0xFF, chipId = bb.getInt(12))
    }

    fun readReg(addr: Int): Int = checkCommand("read register", READ_REG, le(addr)).value

    fun writeReg(addr: Int, value: Int, mask: Int = -1, delayUs: Int = 0) {
        checkCommand("write register", WRITE_REG, le(addr, value, mask, delayUs))
    }

    /**
     * Over USB-Serial-JTAG the RTC watchdog and the super watchdog keep running in
     * download mode and can reset the chip mid-write: switch one off, auto-feed the other
     * (esptool's ESP32C3ROM.disable_watchdogs with the C6's LP_WDT registers).
     */
    fun disableWatchdogs() {
        writeReg(C6_RTC_WDT_WPROTECT, C6_WDT_WKEY)
        writeReg(C6_RTC_WDT_CONFIG0, 0)
        writeReg(C6_RTC_WDT_WPROTECT, 0)
        writeReg(C6_SWD_WPROTECT, C6_WDT_WKEY)
        writeReg(C6_SWD_CONF, readReg(C6_SWD_CONF) or C6_SWD_AUTO_FEED_EN)
        writeReg(C6_SWD_WPROTECT, 0)
    }

    /** Attaches the SPI flash (ROM form: hspi_arg 0, then is_legacy and three reserved bytes). */
    fun spiAttach() {
        checkCommand("attach flash", SPI_ATTACH, le(0) + byteArrayOf(0, 0, 0, 0))
    }

    fun spiSetParams(flashSize: Int) {
        checkCommand("set flash size", SPI_SET_PARAMS, le(0, flashSize, 64 * 1024, 4 * 1024, 256, 0xFFFF))
    }

    // --- flash ----------------------------------------------------------------------------

    /** MD5 of [size] bytes of flash at [addr], as lowercase hex (the ROM sends it as text). */
    fun flashMd5(addr: Int, size: Int): String {
        val r = checkCommand(
            "read back", SPI_FLASH_MD5, le(addr, size, 0, 0),
            respLen = 32, timeoutMs = timeoutPerMb(MD5_MS_PER_MB, size),
        )
        return String(r.body, Charsets.US_ASCII).lowercase()
    }

    /**
     * Writes [data] at [offset] compressed. The ROM erases the whole range on BEGIN and
     * writes each block before it answers. [onBlock] gets (blocks sent, block count).
     * No FLASH_DEFL_END: to the ROM that means "leave the loader and run".
     */
    fun writeFlash(offset: Int, data: ByteArray, onBlock: (Int, Int) -> Unit = { _, _ -> }) {
        val comp = deflate(data)
        val numBlocks = (comp.size + FLASH_WRITE_SIZE - 1) / FLASH_WRITE_SIZE
        val eraseSize = (data.size + FLASH_WRITE_SIZE - 1) / FLASH_WRITE_SIZE * FLASH_WRITE_SIZE
        checkCommand(
            "erase", FLASH_DEFL_BEGIN, le(eraseSize, numBlocks, FLASH_WRITE_SIZE, offset, 0),
            timeoutMs = timeoutPerMb(ERASE_MS_PER_MB, eraseSize),
        )
        // esptool times each block by the bytes it inflates to, so inflate alongside.
        val inflater = Inflater()
        val sink = ByteArray(64 * 1024)
        try {
            for (seq in 0 until numBlocks) {
                val block = comp.copyOfRange(seq * FLASH_WRITE_SIZE, min(comp.size, (seq + 1) * FLASH_WRITE_SIZE))
                inflater.setInput(block)
                var produced = 0
                while (true) {
                    val n = inflater.inflate(sink)
                    if (n == 0) break
                    produced += n
                }
                val timeout = timeoutPerMb(ERASE_WRITE_MS_PER_MB, produced)
                val pkt = le(block.size, seq, 0, 0) + block
                var attempt = 0
                while (true) {
                    try {
                        checkCommand("write block $seq", FLASH_DEFL_DATA, pkt, checksum(block), timeoutMs = timeout)
                        break
                    } catch (e: FlashError) {
                        if (++attempt >= WRITE_BLOCK_ATTEMPTS) throw e
                    }
                }
                onBlock(seq + 1, numBlocks)
            }
        } finally {
            inflater.end()
        }
    }

    // --- resets over USB-Serial-JTAG ------------------------------------------------------

    /** Into download mode (esp-pylib usb_jtag_bootloader_reset; walks through (1,1)). */
    fun usbJtagBootloaderReset() {
        port.setLines(dtr = false, rts = false)
        clock.sleep(100)
        port.setLines(dtr = true, rts = false)
        clock.sleep(100)
        port.setLines(dtr = true, rts = true)
        port.setLines(dtr = false, rts = true)
        clock.sleep(100)
        port.setLines(dtr = false, rts = false)
    }

    /** Restarts into the firmware (esptool HardReset over USB: RTS held 0.2 s). */
    fun hardReset() {
        port.setLines(dtr = false, rts = true)
        clock.sleep(200)
        port.setLines(dtr = false, rts = false)
        clock.sleep(200)
    }

    companion object {
        const val SYNC = 0x08
        const val WRITE_REG = 0x09
        const val READ_REG = 0x0A
        const val SPI_SET_PARAMS = 0x0B
        const val SPI_ATTACH = 0x0D
        const val FLASH_DEFL_BEGIN = 0x10
        const val FLASH_DEFL_DATA = 0x11
        const val FLASH_DEFL_END = 0x12
        const val SPI_FLASH_MD5 = 0x13
        const val GET_SECURITY_INFO = 0x14

        const val FLASH_WRITE_SIZE = 0x400 // ROM block size
        const val ROM_INVALID_RECV_MSG = 0x05
        private const val CHECKSUM_MAGIC = 0xEF
        private const val WRITE_BLOCK_ATTEMPTS = 3

        const val DEFAULT_TIMEOUT = 3_000L
        const val SYNC_TIMEOUT = 100L
        private const val ERASE_MS_PER_MB = 40_000L
        private const val ERASE_WRITE_MS_PER_MB = 40_000L
        private const val MD5_MS_PER_MB = 8_000L

        const val CHIP_ID_C6 = 13

        // ESP32-C6 LP_WDT registers (esptool targets/esp32c6.py)
        private const val C6_LP_WDT = 0x600B1C00
        private const val C6_RTC_WDT_CONFIG0 = C6_LP_WDT + 0x00
        private const val C6_RTC_WDT_WPROTECT = C6_LP_WDT + 0x18
        private const val C6_SWD_CONF = C6_LP_WDT + 0x1C
        private const val C6_SWD_WPROTECT = C6_LP_WDT + 0x20
        private const val C6_WDT_WKEY = 0x50D83AA1
        private const val C6_SWD_AUTO_FEED_EN = 1 shl 18

        // Status error codes of the ROM loader (esptool docs, "Serial Protocol")
        private val ROM_ERRORS = mapOf(
            0x05 to "invalid message",
            0x06 to "failed to act on the message",
            0x07 to "invalid checksum",
            0x08 to "flash write error",
            0x09 to "flash read error",
            0x0A to "flash read length error",
            0x0B to "deflate error",
        )

        /** esptool's timeout_per_mb: [msPerMb] scaled to [size], at least the default. */
        fun timeoutPerMb(msPerMb: Long, size: Int): Long = max(DEFAULT_TIMEOUT, msPerMb * size / 1_000_000)

        fun checksum(data: ByteArray): Int {
            var state = CHECKSUM_MAGIC
            for (b in data) state = state xor (b.toInt() and 0xFF)
            return state
        }

        /** zlib stream at level 9, as esptool's zlib.compress(image, 9). */
        fun deflate(data: ByteArray): ByteArray {
            val d = Deflater(9)
            d.setInput(data)
            d.finish()
            val out = ByteArrayOutputStream(data.size / 2 + 64)
            val chunk = ByteArray(64 * 1024)
            while (!d.finished()) out.write(chunk, 0, d.deflate(chunk))
            d.end()
            return out.toByteArray()
        }

        fun le(vararg words: Int): ByteArray {
            val bb = ByteBuffer.allocate(4 * words.size).order(ByteOrder.LITTLE_ENDIAN)
            for (w in words) bb.putInt(w)
            return bb.array()
        }
    }
}
