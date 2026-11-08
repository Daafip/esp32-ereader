package io.github.daafip.fwswitch

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

class FlasherTest {
    private val clock = FakeClock()
    private val rom = FakeRom(clock)

    private class Recorder(var confirm: Boolean = true) : FlashListener {
        val steps = ArrayList<Step>()
        var asked = 0
        var lastDone = 0L
        var lastTotal = 0L
        override fun onStep(step: Step) {
            steps += step
        }
        override fun onProgress(done: Long, total: Long) {
            assertTrue("progress went back", done >= 0 && done <= total)
            lastDone = done
            lastTotal = total
        }
        override fun confirmWholeImage(): Boolean {
            asked++
            return confirm
        }
    }

    private fun flasher(listener: FlashListener, opener: PortOpener = PortOpener { rom }) = Flasher(opener, listener, clock)

    private fun slice(b: ByteArray, from: Int, to: Int) = b.copyOfRange(from, to)

    /** The board as it is now: the reader (flash build), settings in nvs, books in flash. */
    private fun boardWithReader(reader: FirmwareImage = TestImages.reader()) {
        reader.bytes.copyInto(rom.flash, 0)
        rom.flash.fill(0x11, 0x9000, 0xE000)
        rom.flash.fill(0x22, 0x1F0000, 0x210000)
    }

    @Test
    fun switchesToEsphomeKeepingSettingsAndBooks() {
        boardWithReader()
        val esphome = TestImages.esphomeOnReaderTable()
        val r = Recorder()
        assertTrue(flasher(r).flash(esphome))

        assertEquals(0, r.asked)
        assertArrayEquals(slice(esphome.bytes, 0, 0x9000), slice(rom.flash, 0, 0x9000))
        assertArrayEquals(slice(esphome.bytes, 0xE000, esphome.size), slice(rom.flash, 0xE000, esphome.size))
        assertTrue(slice(rom.flash, 0x9000, 0xE000).all { it == 0x11.toByte() })
        assertTrue(slice(rom.flash, 0x1F0000, 0x210000).all { it == 0x22.toByte() })
        assertEquals(1, rom.bootloaderResets) // the firmware ran: reset over USB-Serial-JTAG
        assertEquals(1, rom.hardResets)
        assertFalse(rom.downloadMode)
        assertFalse(EspLoader.FLASH_DEFL_END in rom.commands) // that would leave the ROM loader
        assertEquals(4 shl 20, rom.flashSizeSet)
        assertEquals(r.lastTotal, r.lastDone)
        assertEquals(Step.DONE, r.steps.last())
    }

    @Test
    fun andBackToTheReader() {
        val reader = TestImages.reader()
        boardWithReader(reader)
        val esphome = TestImages.esphomeOnReaderTable()
        assertTrue(flasher(Recorder()).flash(esphome, listOf(reader)))
        assertTrue(flasher(Recorder()).flash(reader, listOf(esphome)))
        assertArrayEquals(slice(reader.bytes, 0xE000, reader.size), slice(rom.flash, 0xE000, reader.size))
        assertTrue(slice(rom.flash, 0x9000, 0xE000).all { it == 0x11.toByte() })
    }

    /** The real flash-build image (pixi run fw-build): to ESPHome and back. */
    @Test
    fun realReaderImageRoundTrip() {
        val f = java.io.File("../../firmware/.pio/build/reader_firebeetle75_flash/firmware.factory.bin")
        org.junit.Assume.assumeTrue("no firmware build", f.exists())
        val reader = FirmwareImage.parse(f.readBytes())
        boardWithReader(reader)
        val esphome = FirmwareImage.parse(TestImages.merged(TestImages.readerTable, appSize = 1_400_000))
        assertTrue(flasher(Recorder()).flash(esphome, listOf(reader)))
        assertTrue(flasher(Recorder()).flash(reader, listOf(esphome)))
        assertArrayEquals(slice(reader.bytes, 0xE000, reader.size), slice(rom.flash, 0xE000, reader.size))
        assertTrue(slice(rom.flash, 0x9000, 0xE000).all { it == 0x11.toByte() })
        assertTrue(slice(rom.flash, 0x1F0000, 0x210000).all { it == 0x22.toByte() })
    }

    @Test
    fun skipsWhatTheBoardAlreadyHas() {
        val reader = TestImages.reader()
        boardWithReader(reader)
        rom.commands.clear()
        assertTrue(flasher(Recorder()).flash(reader))
        assertFalse(EspLoader.FLASH_DEFL_BEGIN in rom.commands)
        assertEquals(1, rom.hardResets)
    }

    @Test
    fun otherLayoutAsksFirstAndStopsOnNo() {
        boardWithReader()
        val before = rom.flash.copyOf()
        val r = Recorder(confirm = false)
        assertFalse(flasher(r).flash(TestImages.esphomeOwnTable()))
        assertEquals(1, r.asked)
        assertFalse(EspLoader.FLASH_DEFL_BEGIN in rom.commands)
        assertArrayEquals(before, rom.flash)
        assertEquals(1, rom.hardResets) // back to the reader
    }

    @Test
    fun otherLayoutWritesWholeImageAndBlanksItsNvs() {
        boardWithReader()
        rom.flash.fill(0x33, 0x390000, 0x3A0000) // reader books where ESPHome keeps nvs
        val esphome = TestImages.esphomeOwnTable()
        val r = Recorder(confirm = true)
        assertTrue(flasher(r).flash(esphome))
        assertEquals(1, r.asked)
        assertArrayEquals(esphome.bytes, slice(rom.flash, 0, esphome.size))
        assertTrue(slice(rom.flash, 0x390000, 0x400000).all { it == 0xFF.toByte() })
    }

    @Test
    fun waitsForBootAndResetWhenTheBoardSleeps() {
        boardWithReader()
        val appearsAt = 20_000L
        val opener = PortOpener { wait ->
            if (clock.t < appearsAt) {
                clock.t += wait
                null
            } else {
                rom.downloadMode = true // the user held BOOT and pressed RST
                rom
            }
        }
        val r = Recorder()
        assertTrue(flasher(r, opener).flash(TestImages.esphomeOnReaderTable()))
        assertTrue(Step.WAITING_FOR_BOOT_MODE in r.steps)
        assertEquals(0, rom.bootloaderResets)
    }

    @Test
    fun givesUpWhenDownloadModeNeverComes() {
        boardWithReader()
        rom.ignoreResets = true
        try {
            flasher(Recorder()).flash(TestImages.esphomeOnReaderTable())
            fail("flashed")
        } catch (e: FlashError) {
            assertEquals(Reason.NO_BOOT_MODE, e.reason)
        }
    }

    @Test
    fun refusesAnotherChip() {
        rom.downloadMode = true
        rom.chipId = 9
        try {
            flasher(Recorder()).flash(TestImages.reader())
            fail("flashed")
        } catch (e: FlashError) {
            assertEquals(Reason.WRONG_CHIP, e.reason)
        }
    }

    @Test
    fun reportsBadReadBack() {
        boardWithReader()
        rom.corruptWrites = true
        try {
            flasher(Recorder()).flash(TestImages.esphomeOnReaderTable())
            fail("flashed")
        } catch (e: FlashError) {
            assertEquals(Reason.VERIFY, e.reason)
        }
    }

    @Test
    fun switchesWatchdogsOff() {
        rom.downloadMode = true
        rom.regs[0x600B1C00] = 0x12345 // RTC WDT config0
        flasher(Recorder()).flash(TestImages.reader())
        assertEquals(0, rom.regs[0x600B1C00])
        assertEquals(1 shl 18, (rom.regs[0x600B1C1C] ?: 0) and (1 shl 18)) // SWD auto-feed
    }
}
