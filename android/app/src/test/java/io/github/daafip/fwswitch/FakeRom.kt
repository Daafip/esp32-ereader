package io.github.daafip.fwswitch

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.zip.Inflater

class FakeClock : Clock {
    var t = 0L
    override fun now() = t
    override fun sleep(ms: Long) {
        t += ms
    }
}

/**
 * An ESP32-C6 in the ROM loader, behind its USB-Serial-JTAG: answers the commands the
 * app uses, keeps a 4 MB flash (erase sets 0xFF, programming can only clear bits, as NOR
 * flash), and follows the DTR/RTS reset sequences.
 */
class FakeRom(private val clock: FakeClock, flashSize: Int = 4 shl 20) : SerialPort {
    val flash = ByteArray(flashSize) { 0xFF.toByte() }
    var downloadMode = false
    var chipId = EspLoader.CHIP_ID_C6
    var ignoreResets = false
    var corruptWrites = false

    val commands = ArrayList<Int>()
    var bootloaderResets = 0
    var hardResets = 0
    val regs = HashMap<Int, Int>()
    var flashSizeSet = 0

    private val decoder = Slip.Decoder()
    private val out = ArrayDeque<Byte>()
    private var inflater: Inflater? = null
    private var writePos = 0
    private var eraseEnd = 0
    private var expectSeq = 0
    private var dtr = false
    private var rts = false
    private var sawDtr = false

    override fun write(data: ByteArray) {
        for (f in decoder.feed(data)) if (downloadMode) handle(f)
    }

    override fun read(buf: ByteArray, timeoutMs: Int): Int {
        if (out.isEmpty()) {
            clock.t += timeoutMs
            return 0
        }
        var n = 0
        while (n < buf.size && out.isNotEmpty()) buf[n++] = out.removeFirst()
        return n
    }

    /** RTS released after DTR was asserted since idle: download mode; without DTR: normal boot. */
    override fun setLines(dtr: Boolean, rts: Boolean) {
        if (dtr) sawDtr = true
        val released = this.rts && !rts
        this.dtr = dtr
        this.rts = rts
        if (released && !ignoreResets) {
            if (sawDtr) {
                downloadMode = true
                bootloaderResets++
            } else {
                downloadMode = false
                hardResets++
            }
            decoder.reset()
            out.clear()
            inflater = null
        }
        if (!dtr && !rts) sawDtr = false
    }

    override fun close() {}

    private fun reply(op: Int, value: Int = 0, data: ByteArray = ByteArray(0), error: Int = 0) {
        val body = data + byteArrayOf(if (error != 0) 1 else 0, error.toByte(), 0, 0)
        val f = ByteBuffer.allocate(8 + body.size).order(ByteOrder.LITTLE_ENDIAN)
            .put(1).put(op.toByte()).putShort(body.size.toShort()).putInt(value).put(body)
        for (b in Slip.encode(f.array())) out.addLast(b)
    }

    private fun handle(f: ByteArray) {
        val bb = ByteBuffer.wrap(f).order(ByteOrder.LITTLE_ENDIAN)
        require(f[0].toInt() == 0) { "not a command" }
        val op = f[1].toInt() and 0xFF
        val len = bb.getShort(2).toInt() and 0xFFFF
        val chk = bb.getInt(4)
        require(f.size == 8 + len) { "length field $len, frame ${f.size}" }
        val d = ByteBuffer.wrap(f, 8, len).slice().order(ByteOrder.LITTLE_ENDIAN)
        commands.add(op)
        when (op) {
            EspLoader.SYNC -> repeat(8) { reply(op, value = 0x20120707) }
            EspLoader.GET_SECURITY_INFO -> {
                val info = ByteBuffer.allocate(20).order(ByteOrder.LITTLE_ENDIAN).putInt(0).put(ByteArray(8)).putInt(chipId).putInt(0)
                reply(op, data = info.array())
            }
            EspLoader.READ_REG -> reply(op, value = regs[d.getInt(0)] ?: 0)
            EspLoader.WRITE_REG -> {
                val addr = d.getInt(0)
                val mask = d.getInt(8)
                regs[addr] = ((regs[addr] ?: 0) and mask.inv()) or (d.getInt(4) and mask)
                reply(op)
            }
            EspLoader.SPI_ATTACH -> reply(op, error = if (len == 8) 0 else 0x05)
            EspLoader.SPI_SET_PARAMS -> {
                flashSizeSet = d.getInt(4)
                reply(op, error = if (len == 24) 0 else 0x05)
            }
            EspLoader.FLASH_DEFL_BEGIN -> {
                // The C6 ROM takes five words (the last: encrypted write)
                if (len != 20) return reply(op, error = 0x05)
                val eraseSize = d.getInt(0)
                val offset = d.getInt(12)
                eraseEnd = (offset + eraseSize + 0xFFF) / 0x1000 * 0x1000
                for (i in offset / 0x1000 * 0x1000 until eraseEnd) flash[i] = 0xFF.toByte()
                inflater = Inflater()
                writePos = offset
                expectSeq = 0
                reply(op)
            }
            EspLoader.FLASH_DEFL_DATA -> {
                val n = d.getInt(0)
                val seq = d.getInt(4)
                val block = f.copyOfRange(8 + 16, 8 + 16 + n)
                val inf = inflater ?: return reply(op, error = 0x06)
                if (EspLoader.checksum(block) != chk) return reply(op, error = 0x07)
                if (seq != expectSeq) return reply(op, error = 0x05)
                inf.setInput(block)
                val sink = ByteArray(64 * 1024)
                while (true) {
                    val k = inf.inflate(sink)
                    if (k == 0) break
                    if (writePos + k > eraseEnd) return reply(op, error = 0x08)
                    for (i in 0 until k) flash[writePos + i] = (flash[writePos + i].toInt() and sink[i].toInt()).toByte()
                    if (corruptWrites) flash[writePos] = (flash[writePos].toInt() xor 0x01).toByte()
                    writePos += k
                }
                expectSeq++
                reply(op)
            }
            EspLoader.SPI_FLASH_MD5 -> {
                val hex = FirmwareImage.md5Hex(flash, d.getInt(0), d.getInt(4))
                reply(op, data = hex.toByteArray(Charsets.US_ASCII))
            }
            EspLoader.FLASH_DEFL_END -> reply(op)
            else -> reply(op, error = 0x05)
        }
    }
}
