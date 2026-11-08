package io.github.daafip.fwswitch

import android.app.Activity
import android.app.AlertDialog
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.SharedPreferences
import android.content.res.ColorStateList
import android.hardware.usb.UsbManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.OpenableColumns
import android.text.format.Formatter
import android.view.View
import android.view.WindowInsets
import android.view.WindowManager
import android.widget.Button
import android.widget.ProgressBar
import android.widget.TextView
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.IOException
import java.util.EnumMap
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Two firmware files (the e-reader and ESPHome), each kept in the app, and a button
 * each to put it on the board over USB. All USB and file work runs on one worker thread.
 */
class MainActivity : Activity() {
    private enum class Slot(val file: String, val title: Int, val hint: Int) {
        READER("reader.bin", R.string.reader, R.string.reader_hint),
        ESPHOME("esphome.bin", R.string.esphome, R.string.esphome_hint),
    }

    private val images = EnumMap<Slot, FirmwareImage>(Slot::class.java) // touched on the UI thread only
    private val worker = Executors.newSingleThreadExecutor()
    private lateinit var prefs: SharedPreferences
    private lateinit var opener: UsbPortOpener
    private var flasher: Flasher? = null
    private var busy = false
    private var writing = false
    private val logLines = ArrayDeque<String>()

    private lateinit var usb: TextView
    private lateinit var last: TextView
    private lateinit var step: TextView
    private lateinit var stepColors: ColorStateList
    private lateinit var progress: ProgressBar
    private lateinit var cancel: Button
    private lateinit var log: TextView
    private lateinit var fileViews: Map<Slot, TextView>
    private lateinit var installButtons: Map<Slot, Button>
    private lateinit var chooseButtons: Map<Slot, Button>

    private val usbEvents = object : BroadcastReceiver() {
        override fun onReceive(c: Context, intent: Intent) = showUsb()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        // Edge to edge on Android 11-14 too (15+ always is)
        @Suppress("DEPRECATION")
        if (Build.VERSION.SDK_INT >= 30) window.setDecorFitsSystemWindows(false)
        findViewById<View>(R.id.root).setOnApplyWindowInsetsListener { v, insets ->
            if (Build.VERSION.SDK_INT >= 30) {
                val b = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout())
                v.setPadding(b.left, b.top, b.right, b.bottom)
            } else {
                @Suppress("DEPRECATION")
                v.setPadding(insets.systemWindowInsetLeft, insets.systemWindowInsetTop, insets.systemWindowInsetRight, insets.systemWindowInsetBottom)
            }
            insets
        }

        prefs = getSharedPreferences("fwswitch", MODE_PRIVATE)
        opener = UsbPortOpener(this)
        usb = findViewById(R.id.usb)
        last = findViewById(R.id.last)
        step = findViewById(R.id.step)
        stepColors = step.textColors
        progress = findViewById(R.id.progress)
        cancel = findViewById(R.id.cancel)
        log = findViewById(R.id.log)
        fileViews = mapOf(Slot.READER to findViewById(R.id.readerFile), Slot.ESPHOME to findViewById(R.id.esphomeFile))
        installButtons = mapOf(Slot.READER to findViewById(R.id.readerInstall), Slot.ESPHOME to findViewById(R.id.esphomeInstall))
        chooseButtons = mapOf(Slot.READER to findViewById(R.id.readerChoose), Slot.ESPHOME to findViewById(R.id.esphomeChoose))
        for (s in Slot.entries) {
            installButtons.getValue(s).setOnClickListener { install(s) }
            chooseButtons.getValue(s).setOnClickListener { choose(s) }
        }
        cancel.setOnClickListener { flasher?.cancelled = true }

        val filter = IntentFilter().apply {
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
        }
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(usbEvents, filter, RECEIVER_NOT_EXPORTED) else registerReceiver(usbEvents, filter)

        showUsb()
        showLast()
        showSlots()
        busy = true
        updateButtons()
        worker.execute {
            val loaded = Slot.entries.mapNotNull { s -> loadSlot(s)?.let { s to it } }
            runOnUiThread {
                images.putAll(loaded)
                busy = false
                showSlots()
            }
        }
    }

    override fun onResume() {
        super.onResume()
        showUsb()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        showUsb()
    }

    override fun onDestroy() {
        unregisterReceiver(usbEvents)
        worker.shutdown() // a running install still finishes
        super.onDestroy()
    }

    // --- display --------------------------------------------------------------------------

    private fun showUsb() = usb.setText(if (opener.device() != null) R.string.usb_present else R.string.usb_none)

    private fun showLast() {
        val l = prefs.getString("last", null)
        last.visibility = if (l == null) View.GONE else View.VISIBLE
        if (l != null) last.text = getString(R.string.last_installed, l)
    }

    private fun showSlots() {
        for (s in Slot.entries) {
            val img = images[s]
            fileViews.getValue(s).text = if (img == null) getString(s.hint) else describe(s, img)
        }
        updateButtons()
    }

    private fun describe(s: Slot, img: FirmwareImage): String {
        val name = prefs.getString("name_${s.name}", null) ?: s.file
        val size = Formatter.formatShortFileSize(this, img.size.toLong())
        val text = if (img.readerBoard != null) {
            getString(R.string.file_reader, name, img.readerBoard, size)
        } else {
            getString(R.string.file_other, name, img.projectName, img.appVersion, size)
        }
        val warning = when {
            s == Slot.READER && img.readerBoard == null -> getString(R.string.warn_not_reader)
            s == Slot.ESPHOME && img.readerBoard != null -> getString(R.string.warn_is_reader)
            else -> null
        }
        return if (warning == null) text else "$text\n$warning"
    }

    private fun updateButtons() {
        for (s in Slot.entries) {
            installButtons.getValue(s).isEnabled = !busy && images[s] != null
            chooseButtons.getValue(s).isEnabled = !busy
        }
    }

    private fun showStep(text: String, error: Boolean = false) {
        step.text = text
        if (error) step.setTextColor(getColor(R.color.error)) else step.setTextColor(stepColors)
    }

    private fun appendLog(line: String) {
        logLines.addLast(line)
        while (logLines.size > 200) logLines.removeFirst()
        log.text = logLines.joinToString("\n")
    }

    private fun errorText(e: FlashError): String = when (e.reason) {
        Reason.BAD_IMAGE -> getString(R.string.err_bad_image, e.detail)
        Reason.NO_BOOT_MODE -> getString(R.string.err_no_boot)
        Reason.USB -> getString(R.string.err_usb, e.detail)
        Reason.WRONG_CHIP -> getString(R.string.err_wrong_chip, e.detail)
        Reason.ROM -> getString(R.string.err_rom, e.detail)
        Reason.TIMEOUT -> getString(R.string.err_timeout, e.detail)
        Reason.VERIFY -> getString(R.string.err_verify, e.detail)
        Reason.CANCELLED -> getString(R.string.err_cancelled)
    }

    // --- files ----------------------------------------------------------------------------

    private fun slotFile(s: Slot) = File(filesDir, s.file)

    /** The kept file of [s], or null (none, or no longer readable: then it goes). */
    private fun loadSlot(s: Slot): FirmwareImage? {
        val f = slotFile(s)
        if (!f.exists()) return null
        return try {
            FirmwareImage.parse(f.readBytes())
        } catch (e: FlashError) {
            f.delete()
            null
        }
    }

    private fun choose(s: Slot) {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*")
        @Suppress("DEPRECATION")
        startActivityForResult(intent, s.ordinal)
    }

    @Deprecated("Activity result API of the framework Activity")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        @Suppress("DEPRECATION")
        super.onActivityResult(requestCode, resultCode, data)
        val uri = data?.data
        val s = Slot.entries.getOrNull(requestCode)
        if (resultCode != RESULT_OK || uri == null || s == null) return
        busy = true
        updateButtons()
        worker.execute {
            var img: FirmwareImage? = null
            val error = try {
                val bytes = readUri(uri)
                img = FirmwareImage.parse(bytes)
                slotFile(s).writeBytes(bytes)
                prefs.edit().putString("name_${s.name}", displayName(uri) ?: s.file).apply()
                null
            } catch (e: FlashError) {
                errorText(e)
            } catch (e: IOException) {
                getString(R.string.err_other, e.message ?: e.toString())
            }
            runOnUiThread {
                busy = false
                img?.let { images[s] = it }
                showSlots()
                showStep(error ?: "", error != null)
            }
        }
    }

    private fun readUri(uri: Uri): ByteArray {
        val input = contentResolver.openInputStream(uri) ?: throw IOException("can't open the file")
        input.use {
            val out = ByteArrayOutputStream()
            val chunk = ByteArray(64 * 1024)
            while (true) {
                val n = it.read(chunk)
                if (n < 0) break
                out.write(chunk, 0, n)
                if (out.size() > MAX_FILE) throw IOException(getString(R.string.file_too_big))
            }
            return out.toByteArray()
        }
    }

    private fun displayName(uri: Uri): String? =
        contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use {
            if (it.moveToFirst()) it.getString(0) else null
        }

    // --- installing -----------------------------------------------------------------------

    private fun install(s: Slot) {
        val img = images[s] ?: return
        val others = images.filterKeys { it != s }.values.toList()
        busy = true
        updateButtons()
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        logLines.clear()
        log.text = ""
        progress.progress = 0
        progress.visibility = View.GONE
        appendLog("${getString(s.title)}: ${prefs.getString("name_${s.name}", s.file)}")
        val f = Flasher(opener, listener)
        flasher = f
        worker.execute {
            var error = false
            val result = try {
                if (f.flash(img, others)) {
                    prefs.edit().putString("last", getString(s.title)).apply()
                    getString(R.string.step_done, getString(s.title))
                } else {
                    getString(R.string.step_declined)
                }
            } catch (e: FlashError) {
                error = true
                listener.log("error: ${e.message}")
                errorText(e)
            } catch (e: Exception) {
                error = true
                listener.log("error: $e")
                getString(R.string.err_other, e.toString())
            }
            runOnUiThread {
                busy = false
                writing = false
                flasher = null
                window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                cancel.visibility = View.GONE
                updateButtons()
                showStep(result, error)
                showLast()
                showUsb()
            }
        }
    }

    private val listener = object : FlashListener {
        override fun onStep(step: Step) = runOnUiThread {
            writing = step == Step.WRITING
            cancel.visibility = when (step) {
                Step.CONNECTING, Step.RESETTING, Step.WAITING_FOR_BOOT_MODE -> View.VISIBLE
                else -> View.GONE
            }
            if (step == Step.WRITING) progress.visibility = View.VISIBLE
            when (step) {
                Step.CONNECTING -> showStep(getString(R.string.step_connecting))
                Step.RESETTING -> showStep(getString(R.string.step_resetting))
                Step.WAITING_FOR_BOOT_MODE -> showStep(getString(R.string.step_waiting))
                Step.CHECKING -> showStep(getString(R.string.step_checking))
                Step.WRITING -> showStep(getString(R.string.step_writing, progress.progress / 10))
                Step.VERIFYING -> showStep(getString(R.string.step_verifying))
                Step.RESTARTING -> showStep(getString(R.string.step_restarting))
                Step.DONE -> {} // install() shows the result
            }
        }

        override fun onProgress(done: Long, total: Long) = runOnUiThread {
            if (total > 0) progress.progress = (done * 1000 / total).toInt()
            if (writing) step.text = getString(R.string.step_writing, progress.progress / 10)
        }

        override fun log(line: String) = runOnUiThread { appendLog(line) }

        override fun confirmWholeImage(): Boolean {
            val answered = CountDownLatch(1)
            val yes = AtomicBoolean(false)
            runOnUiThread {
                if (isFinishing || isDestroyed) {
                    answered.countDown()
                    return@runOnUiThread
                }
                AlertDialog.Builder(this@MainActivity)
                    .setTitle(R.string.confirm_title)
                    .setMessage(R.string.confirm_message)
                    .setPositiveButton(R.string.confirm_yes) { _, _ -> yes.set(true) }
                    .setNegativeButton(R.string.cancel, null)
                    .setOnDismissListener { answered.countDown() }
                    .show()
            }
            answered.await()
            return yes.get()
        }
    }

    companion object {
        private const val MAX_FILE = 4 shl 20
    }
}
