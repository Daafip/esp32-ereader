package io.github.daafip.fwswitch

import java.io.Closeable

/** A serial link to the board: the C6's USB-Serial-JTAG on the phone, a fake ROM in tests. */
interface SerialPort : Closeable {
    fun write(data: ByteArray)

    /** Reads what arrives within [timeoutMs] into [buf]; returns the count, 0 on timeout. */
    fun read(buf: ByteArray, timeoutMs: Int): Int

    /** Sets DTR and RTS (true = asserted, as pyserial's setDTR/setRTS). */
    fun setLines(dtr: Boolean, rts: Boolean)
}

/** Opens the board's port, waiting for it to (re)appear on USB. */
fun interface PortOpener {
    /** The port, or null when no board showed up within [waitMs]. */
    fun open(waitMs: Long): SerialPort?
}

/** Time, swappable in tests. */
interface Clock {
    fun now(): Long
    fun sleep(ms: Long)

    object System : Clock {
        override fun now() = java.lang.System.nanoTime() / 1_000_000
        override fun sleep(ms: Long) = Thread.sleep(ms)
    }
}
