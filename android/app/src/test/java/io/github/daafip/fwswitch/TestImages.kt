package io.github.daafip.fwswitch

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import kotlin.random.Random

/** Synthetic full (merged) ESP32-C6 images. */
object TestImages {
    /** firmware/partitions-fb75-flash.csv: the reader without a card. */
    val readerTable = listOf(
        Partition(1, 0x02, 0x9000, 0x5000, "nvs", 0),
        Partition(0, 0x00, 0x10000, 0x1E0000, "app0", 0),
        Partition(1, 0x82, 0x1F0000, 0x200000, "books", 0),
        Partition(1, 0x03, 0x3F0000, 0x10000, "coredump", 0),
    )

    /** ESPHome's own 4 MB layout for esp-idf builds (components/esp32 get_partition_csv). */
    val esphomeTable = listOf(
        Partition(1, 0x00, 0x9000, 0x2000, "otadata", 0),
        Partition(1, 0x01, 0xB000, 0x1000, "phy_init", 0),
        Partition(0, 0x10, 0x10000, 0x1C0000, "app0", 0),
        Partition(0, 0x11, 0x1D0000, 0x1C0000, "app1", 0),
        Partition(1, 0x02, 0x390000, 0x70000, "nvs", 0),
    )

    fun tableBytes(parts: List<Partition>): ByteArray {
        val b = ByteBuffer.allocate(PartitionTable.LENGTH).order(ByteOrder.LITTLE_ENDIAN)
        for (p in parts) {
            b.putShort(0x50AA.toShort()).put(p.type.toByte()).put(p.subtype.toByte()).putInt(p.offset).putInt(p.size)
            val label = ByteArray(16)
            p.label.toByteArray(Charsets.US_ASCII).copyInto(label)
            b.put(label).putInt(p.flags)
        }
        val entries = b.position()
        val md5 = MessageDigest.getInstance("MD5").apply { update(b.array(), 0, entries) }.digest()
        b.putShort(0xEBEB.toShort()).put(ByteArray(14) { 0xFF.toByte() }).put(md5)
        while (b.hasRemaining()) b.put(0xFF.toByte())
        return b.array()
    }

    fun header(chipId: Int = EspLoader.CHIP_ID_C6): ByteArray {
        val h = ByteBuffer.allocate(24).order(ByteOrder.LITTLE_ENDIAN)
        h.put(0xE9.toByte()).put(3).put(2).put(0x20) // magic, segments, DIO, 4 MB / 40 MHz
        h.putInt(0x40800000).put(0xEE.toByte()).put(ByteArray(3)).putShort(chipId.toShort())
        return h.array()
    }

    /** An app image: header, app description, [tag] somewhere inside, random bytes. */
    fun app(size: Int, project: String, version: String, tag: String? = null, seed: Int = 1, chipId: Int = EspLoader.CHIP_ID_C6): ByteArray {
        val a = Random(seed).nextBytes(size)
        header(chipId).copyInto(a, 0)
        ByteBuffer.wrap(a).order(ByteOrder.LITTLE_ENDIAN).putInt(24, 0x3C000020).putInt(28, 256).putInt(32, 0xABCD5432.toInt())
        fun putString(at: Int, s: String) {
            ByteArray(32).also { s.toByteArray(Charsets.US_ASCII).copyInto(it) }.copyInto(a, at)
        }
        putString(48, version)
        putString(80, project)
        putString(144, "v5.5.4")
        if (tag != null) ("T5READER-BOARD:$tag\u0000").toByteArray(Charsets.US_ASCII).copyInto(a, size / 2)
        return a
    }

    fun merged(
        parts: List<Partition>,
        appSize: Int = 200_000,
        project: String = "epaper-dashboard",
        version: String = "2026.10.2",
        tag: String? = null,
        seed: Int = 1,
    ): ByteArray {
        val appAt = parts.first { it.isApp }.offset
        val img = ByteArray(appAt + appSize) { 0xFF.toByte() }
        val boot = Random(seed + 100).nextBytes(0x5000)
        header().copyInto(boot, 0)
        boot.copyInto(img, 0)
        tableBytes(parts).copyInto(img, PartitionTable.OFFSET)
        app(appSize, project, version, tag, seed).copyInto(img, appAt)
        return img
    }

    fun reader(seed: Int = 7) = FirmwareImage.parse(
        merged(readerTable, appSize = 300_000, project = "arduino-lib-builder", version = "43a8f6d", tag = "firebeetle-75-flash", seed = seed),
    )

    fun esphomeOnReaderTable(seed: Int = 3) = FirmwareImage.parse(merged(readerTable, seed = seed))

    fun esphomeOwnTable(seed: Int = 5) = FirmwareImage.parse(merged(esphomeTable, seed = seed))
}
