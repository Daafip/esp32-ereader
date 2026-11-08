package io.github.daafip.fwswitch

import java.io.ByteArrayOutputStream

/** SLIP framing as the ESP ROM loader uses it: 0xC0 opens and closes a frame, 0xDB escapes. */
object Slip {
    private const val END = 0xC0
    private const val ESC = 0xDB
    private const val ESC_END = 0xDC
    private const val ESC_ESC = 0xDD

    fun encode(payload: ByteArray): ByteArray {
        val out = ByteArrayOutputStream(payload.size + payload.size / 16 + 2)
        out.write(END)
        for (b in payload) {
            when (val v = b.toInt() and 0xFF) {
                END -> { out.write(ESC); out.write(ESC_END) }
                ESC -> { out.write(ESC); out.write(ESC_ESC) }
                else -> out.write(v)
            }
        }
        out.write(END)
        return out.toByteArray()
    }

    /** Collects frames from a byte stream; bytes between frames (boot log text) are dropped. */
    class Decoder {
        private val frame = ByteArrayOutputStream()
        private var inFrame = false
        private var escaped = false

        /** Feeds [len] bytes of [data]; returns the frames they complete. */
        fun feed(data: ByteArray, len: Int = data.size): List<ByteArray> {
            val frames = ArrayList<ByteArray>()
            for (i in 0 until len) {
                val b = data[i].toInt() and 0xFF
                if (!inFrame) {
                    if (b == END) {
                        inFrame = true
                        frame.reset()
                    }
                    continue
                }
                when {
                    escaped -> {
                        escaped = false
                        frame.write(
                            when (b) {
                                ESC_END -> END
                                ESC_ESC -> ESC
                                else -> b
                            },
                        )
                    }
                    b == ESC -> escaped = true
                    // An empty frame is the closing END of one frame followed by the next's opening
                    b == END -> if (frame.size() > 0) {
                        frames.add(frame.toByteArray())
                        inFrame = false
                    }
                    else -> frame.write(b)
                }
            }
            return frames
        }

        fun reset() {
            frame.reset()
            inFrame = false
            escaped = false
        }
    }
}
