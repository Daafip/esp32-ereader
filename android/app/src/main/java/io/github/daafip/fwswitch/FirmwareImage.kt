package io.github.daafip.fwswitch

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest

/** One partition table entry. */
data class Partition(
    val type: Int,
    val subtype: Int,
    val offset: Int,
    val size: Int,
    val label: String,
    val flags: Int,
) {
    val end: Int get() = offset + size
    val isApp: Boolean get() = type == TYPE_APP

    /** Data a firmware keeps between starts (settings, books, ...): every data partition but otadata. */
    val holdsData: Boolean get() = type == TYPE_DATA && subtype != SUBTYPE_OTADATA
    val isNvs: Boolean get() = type == TYPE_DATA && subtype == SUBTYPE_NVS

    companion object {
        const val TYPE_APP = 0
        const val TYPE_DATA = 1
        const val SUBTYPE_FACTORY = 0x00
        const val SUBTYPE_OTA_0 = 0x10
        const val SUBTYPE_OTADATA = 0x00
        const val SUBTYPE_NVS = 0x02
    }
}

/** The binary partition table at 0x8000 (ESP-IDF gen_esp32part format). */
object PartitionTable {
    const val OFFSET = 0x8000
    const val LENGTH = 0xC00
    private const val ENTRY = 32
    private const val MAGIC = 0x50AA
    private const val MD5_MAGIC = 0xEBEB

    fun parse(bytes: ByteArray, at: Int = OFFSET): List<Partition> {
        if (bytes.size < at + LENGTH) throw FlashError(Reason.BAD_IMAGE, "no partition table at 0x%x".format(at))
        val bb = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        val out = ArrayList<Partition>()
        for (i in 0 until LENGTH step ENTRY) {
            val pos = at + i
            when (bb.getShort(pos).toInt() and 0xFFFF) {
                MAGIC -> {
                    val label = String(bytes, pos + 12, 16, Charsets.US_ASCII).substringBefore('\u0000')
                    out.add(
                        Partition(
                            type = bytes[pos + 2].toInt() and 0xFF,
                            subtype = bytes[pos + 3].toInt() and 0xFF,
                            offset = bb.getInt(pos + 4),
                            size = bb.getInt(pos + 8),
                            label = label,
                            flags = bb.getInt(pos + 28),
                        ),
                    )
                }
                MD5_MAGIC -> {
                    val want = bytes.copyOfRange(pos + 16, pos + 32)
                    val md = MessageDigest.getInstance("MD5")
                    md.update(bytes, at, i)
                    if (!md.digest().contentEquals(want)) {
                        throw FlashError(Reason.BAD_IMAGE, "partition table checksum is wrong")
                    }
                    break
                }
                0xFFFF -> break
                else -> throw FlashError(Reason.BAD_IMAGE, "no partition table at 0x%x".format(at))
            }
        }
        if (out.isEmpty()) throw FlashError(Reason.BAD_IMAGE, "empty partition table")
        return out
    }
}

/**
 * A full ("factory", merged) ESP32-C6 image as flashed at 0x0: bootloader, partition table
 * and the app. The reader's fw-dist `firebeetle-75-flash-reader-<v>.bin` and ESPHome's
 * `firmware.factory.bin` are such files.
 */
class FirmwareImage private constructor(
    val bytes: ByteArray,
    val chipId: Int,
    val flashSize: Int,
    val partitions: List<Partition>,
    val tableMd5: String,
    val app: Partition,
    val appVersion: String,
    val projectName: String,
    val idfVersion: String,
    /** BOARD_NAME from the e-reader's `T5READER-BOARD:` tag, null for other firmware. */
    val readerBoard: String?,
) {
    val size: Int get() = bytes.size
    val appBytes: Int get() = bytes.size - app.offset

    companion object {
        private const val IMAGE_MAGIC = 0xE9
        private const val APP_DESC_MAGIC = 0xABCD5432.toInt()
        private const val APP_DESC_AT = 32 // image header (24) + first segment header (8)
        private const val READER_TAG = "T5READER-BOARD:"

        // esp_image_header_t spi_size nibble -> bytes
        private val FLASH_SIZES = intArrayOf(1, 2, 4, 8, 16, 32, 64, 128).map { it shl 20 }

        private val CHIPS = mapOf(
            0 to "ESP32", 2 to "ESP32-S2", 5 to "ESP32-C3", 9 to "ESP32-S3",
            12 to "ESP32-C2", 13 to "ESP32-C6", 16 to "ESP32-H2", 18 to "ESP32-P4",
        )

        fun chipName(id: Int): String = CHIPS[id] ?: "chip $id"

        fun md5Hex(data: ByteArray, from: Int = 0, len: Int = data.size - from): String {
            val md = MessageDigest.getInstance("MD5")
            md.update(data, from, len)
            return md.digest().joinToString("") { "%02x".format(it) }
        }

        private fun u8(b: ByteArray, i: Int) = b[i].toInt() and 0xFF
        private fun u32(b: ByteArray, i: Int) = ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN).getInt(i)
        private fun hasAppDesc(b: ByteArray, at: Int) =
            b.size >= at + 256 && u8(b, at) == IMAGE_MAGIC && u32(b, at + APP_DESC_AT) == APP_DESC_MAGIC

        private fun cString(b: ByteArray, at: Int, len: Int) =
            String(b, at, len, Charsets.US_ASCII).substringBefore('\u0000').trim()

        fun parse(bytes: ByteArray): FirmwareImage {
            if (hasAppDesc(bytes, 0)) {
                throw FlashError(
                    Reason.BAD_IMAGE,
                    "this is an app-only image (-ota.bin, firmware.bin): choose the full image (factory .bin, flashed at 0x0)",
                )
            }
            if (bytes.size < 0x10000 || u8(bytes, 0) != IMAGE_MAGIC) {
                if (bytes.size > 0x1000 && u8(bytes, 0x1000) == IMAGE_MAGIC) {
                    throw FlashError(Reason.WRONG_CHIP, "bootloader at 0x1000: an ESP32 or ESP32-S2 image, not ESP32-C6")
                }
                throw FlashError(Reason.BAD_IMAGE, "not a full ESP firmware image")
            }
            val chipId = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).getShort(12).toInt() and 0xFFFF
            if (chipId != EspLoader.CHIP_ID_C6) {
                throw FlashError(Reason.WRONG_CHIP, "image for ${chipName(chipId)}; this app flashes ESP32-C6 boards")
            }
            val flashSize = FLASH_SIZES.getOrNull(u8(bytes, 3) shr 4)
                ?: throw FlashError(Reason.BAD_IMAGE, "unknown flash size in the image header")

            val partitions = PartitionTable.parse(bytes)
            val apps = partitions.filter { it.isApp }
            val app = apps.firstOrNull { it.subtype == Partition.SUBTYPE_FACTORY }
                ?: apps.firstOrNull { it.subtype == Partition.SUBTYPE_OTA_0 }
                ?: throw FlashError(Reason.BAD_IMAGE, "no app partition in the table")
            if (!hasAppDesc(bytes, app.offset)) {
                throw FlashError(Reason.BAD_IMAGE, "no app at 0x%x".format(app.offset))
            }
            if (bytes.size - app.offset > app.size) {
                throw FlashError(
                    Reason.BAD_IMAGE,
                    "app (%d bytes) is larger than its partition (%d)".format(bytes.size - app.offset, app.size),
                )
            }
            val end = maxOf(bytes.size, partitions.maxOf { it.end })
            if (end > flashSize) throw FlashError(Reason.BAD_IMAGE, "image goes past the end of the flash")

            val desc = app.offset + APP_DESC_AT
            return FirmwareImage(
                bytes = bytes,
                chipId = chipId,
                flashSize = flashSize,
                partitions = partitions,
                tableMd5 = md5Hex(bytes, PartitionTable.OFFSET, PartitionTable.LENGTH),
                app = app,
                appVersion = cString(bytes, desc + 16, 32),
                projectName = cString(bytes, desc + 48, 32),
                idfVersion = cString(bytes, desc + 112, 32),
                readerBoard = findTag(bytes, app.offset),
            )
        }

        /** The name after the first `T5READER-BOARD:` in the app (firmware/src/transfer.cpp). */
        private fun findTag(b: ByteArray, from: Int): String? {
            val tag = READER_TAG.toByteArray(Charsets.US_ASCII)
            var i = from
            outer@ while (i <= b.size - tag.size) {
                for (k in tag.indices) {
                    if (b[i + k] != tag[k]) {
                        i++
                        continue@outer
                    }
                }
                val start = i + tag.size
                var j = start
                while (j < b.size && j - start < 31 && u8(b, j) in 0x21..0x7E) j++
                return if (j > start) String(b, start, j - start, Charsets.US_ASCII) else null
            }
            return null
        }
    }
}

/** A stretch of flash to write: bytes of the image, or blank (0xFF) to clear old data. */
data class Region(val offset: Int, val length: Int, val blank: Boolean = false) {
    val end: Int get() = offset + length
}

/** What to write for an image, given what the board has. */
object Planner {
    /**
     * True when the board's partition table (its MD5) is the image's, or another known
     * image's whose entries are the same: then the data partitions can stay.
     */
    fun sameLayout(boardTableMd5: String, image: FirmwareImage, known: List<FirmwareImage>): Boolean =
        boardTableMd5 == image.tableMd5 ||
            known.any { it.tableMd5 == boardTableMd5 && it.partitions == image.partitions }

    /**
     * Same layout: the image minus its data partitions, so settings (nvs) and the books
     * in flash stay. In a merged image those stretches are 0xFF padding.
     */
    fun keepData(image: FirmwareImage): List<Region> {
        var regions = listOf(Region(0, image.size))
        for (p in image.partitions.filter { it.holdsData }) {
            regions = regions.flatMap { subtract(it, p.offset, p.end) }
        }
        return regions
    }

    /**
     * Another layout: the whole image (its nvs stretch is blank, so settings start fresh),
     * plus blanking the nvs partitions it doesn't reach: they hold the old firmware's bytes.
     */
    fun wholeImage(image: FirmwareImage): List<Region> =
        listOf(Region(0, image.size)) +
            image.partitions.filter { it.isNvs && it.offset >= image.size }.map { Region(it.offset, it.size, blank = true) }

    fun data(image: FirmwareImage, r: Region): ByteArray =
        if (r.blank) ByteArray(r.length) { 0xFF.toByte() } else image.bytes.copyOfRange(r.offset, r.end)

    private fun subtract(r: Region, from: Int, to: Int): List<Region> {
        if (to <= r.offset || from >= r.end) return listOf(r)
        val out = ArrayList<Region>(2)
        if (from > r.offset) out.add(Region(r.offset, from - r.offset))
        if (to < r.end) out.add(Region(to, r.end - to))
        return out
    }
}
