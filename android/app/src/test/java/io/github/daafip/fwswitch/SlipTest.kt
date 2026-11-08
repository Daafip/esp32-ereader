package io.github.daafip.fwswitch

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test

class SlipTest {
    @Test
    fun escapesEndAndEsc() {
        val payload = byteArrayOf(1, 0xC0.toByte(), 2, 0xDB.toByte(), 3)
        val enc = Slip.encode(payload)
        assertArrayEquals(
            byteArrayOf(0xC0.toByte(), 1, 0xDB.toByte(), 0xDC.toByte(), 2, 0xDB.toByte(), 0xDD.toByte(), 3, 0xC0.toByte()),
            enc,
        )
        assertArrayEquals(payload, Slip.Decoder().feed(enc).single())
    }

    @Test
    fun framesSplitAcrossReadsAndNoiseBetween() {
        val a = byteArrayOf(1, 2, 0xC0.toByte())
        val b = byteArrayOf(0xDB.toByte(), 9)
        val stream = "ESP-ROM boot log\r\n".toByteArray() + Slip.encode(a) + Slip.encode(b)
        val d = Slip.Decoder()
        val got = ArrayList<ByteArray>()
        for (i in stream.indices step 3) got += d.feed(stream.copyOfRange(i, minOf(stream.size, i + 3)))
        assertEquals(2, got.size)
        assertArrayEquals(a, got[0])
        assertArrayEquals(b, got[1])
    }
}
