package io.github.daafip.fwswitch

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File

class FirmwareImageTest {
    private fun rejects(reason: Reason, bytes: ByteArray, text: String = "") {
        try {
            FirmwareImage.parse(bytes)
            fail("accepted")
        } catch (e: FlashError) {
            assertEquals(reason, e.reason)
            assertTrue(e.detail, e.detail.contains(text))
        }
    }

    @Test
    fun readsReaderImage() {
        val img = TestImages.reader()
        assertEquals(EspLoader.CHIP_ID_C6, img.chipId)
        assertEquals(4 shl 20, img.flashSize)
        assertEquals(0x10000, img.app.offset)
        assertEquals("firebeetle-75-flash", img.readerBoard)
        assertEquals("arduino-lib-builder", img.projectName)
        assertEquals("v5.5.4", img.idfVersion)
        assertEquals(TestImages.readerTable, img.partitions)
        assertEquals(FirmwareImage.md5Hex(TestImages.tableBytes(TestImages.readerTable)), img.tableMd5)
    }

    @Test
    fun readsEsphomeImage() {
        val img = TestImages.esphomeOwnTable()
        assertNull(img.readerBoard)
        assertEquals("epaper-dashboard", img.projectName)
        assertEquals("2026.10.2", img.appVersion)
        assertEquals(0x10, img.app.subtype) // ota_0
    }

    @Test
    fun refusesAppOnlyImage() {
        rejects(Reason.BAD_IMAGE, TestImages.app(200_000, "x", "1"), "app-only")
    }

    @Test
    fun refusesOtherChips() {
        val esp32 = ByteArray(0x20000) { 0xFF.toByte() }
        TestImages.header(chipId = 0).copyInto(esp32, 0x1000)
        rejects(Reason.WRONG_CHIP, esp32, "0x1000")
        val s3 = TestImages.merged(TestImages.readerTable)
        TestImages.header(chipId = 9).copyInto(s3, 0)
        rejects(Reason.WRONG_CHIP, s3, "ESP32-S3")
    }

    @Test
    fun refusesDamagedTable() {
        val img = TestImages.merged(TestImages.readerTable)
        img[PartitionTable.OFFSET + 5] = 0x7F // offset of the nvs entry
        rejects(Reason.BAD_IMAGE, img, "checksum")
    }

    @Test
    fun refusesAppLargerThanItsPartition() {
        val small = listOf(
            Partition(1, 0x02, 0x9000, 0x5000, "nvs", 0),
            Partition(0, 0x00, 0x10000, 0x10000, "app0", 0),
        )
        rejects(Reason.BAD_IMAGE, TestImages.merged(small, appSize = 0x10001), "larger")
    }

    @Test
    fun plansKeepDataAroundNvs() {
        val img = TestImages.reader()
        assertEquals(listOf(Region(0, 0x9000), Region(0xE000, img.size - 0xE000)), Planner.keepData(img))
    }

    @Test
    fun plansWholeImageAndBlanksDataBeyondIt() {
        val img = TestImages.esphomeOwnTable() // otadata and phy_init lie inside the image
        assertEquals(listOf(Region(0, img.size), Region(0x390000, 0x70000, blank = true)), Planner.wholeImage(img))
        val reader = TestImages.reader() // nvs inside; books and coredump beyond
        assertEquals(
            listOf(
                Region(0, reader.size),
                Region(0x1F0000, 0x200000, blank = true),
                Region(0x3F0000, 0x10000, blank = true),
            ),
            Planner.wholeImage(reader),
        )
    }

    @Test
    fun sameLayoutByEntriesWhenBytesDiffer() {
        val esphome = TestImages.esphomeOnReaderTable()
        // Same entries, other bytes (padding after the MD5 entry): another builder's table.
        val other = TestImages.merged(TestImages.readerTable, seed = 9)
        other[PartitionTable.OFFSET + PartitionTable.LENGTH - 1] = 0
        val reader = FirmwareImage.parse(other)
        assertFalse(reader.tableMd5 == esphome.tableMd5)
        assertTrue(Planner.sameLayout(reader.tableMd5, esphome, listOf(reader)))
        assertFalse(Planner.sameLayout(reader.tableMd5, esphome, emptyList()))
        assertFalse(Planner.sameLayout(TestImages.esphomeOwnTable().tableMd5, esphome, listOf(reader)))
    }

    /** The real flash-build image, when the firmware has been built (pixi run fw-build). */
    @Test
    fun readsRealFlashBuild() {
        val f = File("../../firmware/.pio/build/reader_firebeetle75_flash/firmware.factory.bin")
        assumeTrue("no firmware build", f.exists())
        val img = FirmwareImage.parse(f.readBytes())
        assertEquals("firebeetle-75-flash", img.readerBoard)
        assertEquals(0x10000, img.app.offset)
        assertEquals(TestImages.readerTable, img.partitions)
        assertEquals(FirmwareImage.md5Hex(TestImages.tableBytes(TestImages.readerTable)), img.tableMd5)
        val parts = File("../../firmware/.pio/build/reader_firebeetle75_flash/partitions.bin")
        if (parts.exists()) assertEquals(FirmwareImage.md5Hex(parts.readBytes()), img.tableMd5)
    }
}
