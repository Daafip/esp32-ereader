package io.github.daafip.fwswitch

import android.annotation.SuppressLint
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.SystemClock
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.min

/**
 * The ESP32-C6's USB-Serial-JTAG as a serial port: a CDC-ACM device (Espressif
 * 303A:1001) driven with Android's USB host API. DTR and RTS reach the chip's reset
 * logic through SET_CONTROL_LINE_STATE.
 */
class UsbCdcPort private constructor(
    private val conn: UsbDeviceConnection,
    private val comm: UsbInterface,
    private val data: UsbInterface,
    private val epIn: UsbEndpoint,
    private val epOut: UsbEndpoint,
) : SerialPort {
    override fun write(data: ByteArray) {
        var off = 0
        while (off < data.size) {
            val n = conn.bulkTransfer(epOut, data, off, min(MAX_CHUNK, data.size - off), WRITE_TIMEOUT_MS)
            if (n <= 0) throw FlashError(Reason.USB, "write failed (board gone?)")
            off += n
        }
    }

    // bulkTransfer gives -1 on timeout as on errors; a gone board shows as silence.
    override fun read(buf: ByteArray, timeoutMs: Int): Int =
        conn.bulkTransfer(epIn, buf, buf.size, timeoutMs.coerceAtLeast(1)).coerceAtLeast(0)

    override fun setLines(dtr: Boolean, rts: Boolean) {
        // Fails while the chip resets and leaves the bus; the next state still goes out.
        conn.controlTransfer(
            REQUEST_TYPE_CLASS_OUT, SET_CONTROL_LINE_STATE,
            (if (dtr) 1 else 0) or (if (rts) 2 else 0), comm.id, null, 0, CONTROL_TIMEOUT_MS,
        )
    }

    override fun close() {
        conn.releaseInterface(data)
        conn.releaseInterface(comm)
        conn.close()
    }

    companion object {
        const val VENDOR_ESPRESSIF = 0x303A
        const val PRODUCT_USB_SERIAL_JTAG = 0x1001

        private const val REQUEST_TYPE_CLASS_OUT = 0x21
        private const val SET_LINE_CODING = 0x20
        private const val SET_CONTROL_LINE_STATE = 0x22
        private const val MAX_CHUNK = 16384
        private const val WRITE_TIMEOUT_MS = 2_000
        private const val CONTROL_TIMEOUT_MS = 1_000

        fun matches(d: UsbDevice) = d.vendorId == VENDOR_ESPRESSIF && d.productId == PRODUCT_USB_SERIAL_JTAG

        fun open(manager: UsbManager, device: UsbDevice): UsbCdcPort {
            var comm: UsbInterface? = null
            var data: UsbInterface? = null
            for (i in 0 until device.interfaceCount) {
                val itf = device.getInterface(i)
                when (itf.interfaceClass) {
                    UsbConstants.USB_CLASS_COMM -> if (comm == null) comm = itf
                    UsbConstants.USB_CLASS_CDC_DATA -> if (data == null) data = itf
                }
            }
            if (comm == null || data == null) throw FlashError(Reason.USB, "no serial interface on the device")
            var epIn: UsbEndpoint? = null
            var epOut: UsbEndpoint? = null
            for (i in 0 until data.endpointCount) {
                val ep = data.getEndpoint(i)
                if (ep.type != UsbConstants.USB_ENDPOINT_XFER_BULK) continue
                if (ep.direction == UsbConstants.USB_DIR_IN) epIn = ep else epOut = ep
            }
            if (epIn == null || epOut == null) throw FlashError(Reason.USB, "no bulk endpoints on the device")

            val conn = manager.openDevice(device) ?: throw FlashError(Reason.USB, "can't open the device")
            if (!conn.claimInterface(comm, true) || !conn.claimInterface(data, true)) {
                conn.close()
                throw FlashError(Reason.USB, "can't claim the device")
            }
            // The USB-Serial-JTAG ignores the line coding; set it as a terminal would.
            val coding = byteArrayOf(0x00, 0xC2.toByte(), 0x01, 0x00, 0, 0, 8) // 115200 8N1
            conn.controlTransfer(REQUEST_TYPE_CLASS_OUT, SET_LINE_CODING, 0, comm.id, coding, coding.size, CONTROL_TIMEOUT_MS)
            return UsbCdcPort(conn, comm, data, epIn, epOut)
        }
    }
}

/** Finds the board on USB, asks for permission when needed and opens it (worker thread). */
class UsbPortOpener(private val context: Context) : PortOpener {
    private val manager = context.getSystemService(UsbManager::class.java)

    fun device(): UsbDevice? = manager.deviceList.values.firstOrNull(UsbCdcPort::matches)

    override fun open(waitMs: Long): SerialPort? {
        val deadline = SystemClock.elapsedRealtime() + waitMs
        while (true) {
            val d = device()
            if (d != null) {
                if (!manager.hasPermission(d) && !askPermission(d)) {
                    throw FlashError(Reason.USB, "permission refused")
                }
                try {
                    return UsbCdcPort.open(manager, d)
                } catch (e: FlashError) {
                    // Just re-enumerated: give it a moment
                    if (SystemClock.elapsedRealtime() >= deadline) throw e
                }
            }
            if (SystemClock.elapsedRealtime() >= deadline) return null
            Thread.sleep(200)
        }
    }

    @SuppressLint("InlinedApi") // RECEIVER_NOT_EXPORTED below Android 13 on purpose
    private fun askPermission(d: UsbDevice): Boolean {
        val answered = CountDownLatch(1)
        val granted = AtomicBoolean(false)
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(c: Context, intent: Intent) {
                granted.set(intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false))
                answered.countDown()
            }
        }
        // RECEIVER_NOT_EXPORTED is Android 13+; older versions ignore the bit.
        context.registerReceiver(receiver, IntentFilter(ACTION_PERMISSION), Context.RECEIVER_NOT_EXPORTED)
        try {
            val flags = if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0
            val intent = Intent(ACTION_PERMISSION).setPackage(context.packageName)
            manager.requestPermission(d, PendingIntent.getBroadcast(context, 0, intent, flags))
            answered.await(60, TimeUnit.SECONDS)
        } finally {
            context.unregisterReceiver(receiver)
        }
        return granted.get()
    }

    companion object {
        private const val ACTION_PERMISSION = "io.github.daafip.fwswitch.USB_PERMISSION"
    }
}
