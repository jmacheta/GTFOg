package io.github.jmacheta.gtfog

import android.annotation.SuppressLint
import android.app.Application
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.Intent
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import androidx.core.content.ContextCompat
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import java.io.BufferedWriter
import java.io.File
import java.text.SimpleDateFormat
import java.time.ZonedDateTime
import java.time.format.DateTimeFormatter
import java.util.Date
import java.util.Locale
import java.util.concurrent.Executors

data class ScanItem(val device: BluetoothDevice, val name: String, val address: String, val rssi: Int) {
    val label get() = deviceLabel(name, address)
}

private const val SCAN_TIMEOUT_MS = 30_000L

data class Stats(val samples: Long = 0, val lost: Long = 0)

data class RecState(
    val file: String = "",
    val startMs: Long = 0,
    val endMs: Long = 0,
    val samples: Long = 0,
    val active: Boolean = false,
)

/** Process-wide glue: scanning, the GATT client, the sample ring, loss counters and the CSV recorder. */
@SuppressLint("MissingPermission")
class Session(private val app: Application) {
    val ring = SampleRing(4096)
    val client = GtfogClient(app, ::onFrame, ::onReconnect)
    val dfu = Dfu(app, client)
    val adapter = app.getSystemService(BluetoothManager::class.java)?.adapter
    val scanResults = MutableStateFlow<List<ScanItem>>(emptyList())
    val stats = MutableStateFlow(Stats())
    val rec = MutableStateFlow(RecState())
    val recordings = MutableStateFlow<List<File>>(emptyList())
    val scanning = MutableStateFlow(false)
    val scanError = MutableStateFlow<String?>(null)
    val recError = MutableStateFlow<String?>(null)
    private val prefs = app.getSharedPreferences("gtfog", Context.MODE_PRIVATE)
    private val handler = Handler(Looper.getMainLooper())
    private val scanTimeout = Runnable { stopScan() }

    // Connect by itself to the last goggles only once per app start: after a Disconnect or a failure the user chooses.
    private var autoConnect = true

    val lastAddress: String? get() = prefs.getString("last", null)

    private val tracker = SeqTracker()
    private val io = Executors.newSingleThreadExecutor()
    private var writer: BufferedWriter? = null // only touched on the io thread
    private val found = LinkedHashMap<String, ScanItem>()

    val dir: File
        get() = File(app.getExternalFilesDir(null) ?: app.filesDir, "recordings").apply { mkdirs() }

    init {
        refreshRecordings()
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, r: ScanResult) {
            val name = r.scanRecord?.deviceName ?: r.device.name ?: "GTFOg"
            found[r.device.address] = ScanItem(r.device, name, r.device.address, r.rssi)
            scanResults.value = found.values.sortedByDescending { it.rssi }
            if (autoConnect && r.device.address == lastAddress && client.state.value.conn == Conn.Idle) connect(found.getValue(r.device.address))
        }

        override fun onScanFailed(errorCode: Int) {
            handler.removeCallbacks(scanTimeout)
            scanning.value = false
            scanError.value = "Scan failed (error $errorCode)"
        }
    }

    fun startScan() {
        if (adapter?.isEnabled != true) return
        stopScan()
        scanError.value = null
        found.clear()
        scanResults.value = emptyList()
        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(Uuids.SERVICE)).build()
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        adapter.bluetoothLeScanner?.startScan(listOf(filter), settings, scanCallback)
        scanning.value = true
        handler.postDelayed(scanTimeout, SCAN_TIMEOUT_MS)
    }

    fun stopScan() {
        handler.removeCallbacks(scanTimeout)
        scanning.value = false
        if (adapter?.isEnabled == true) adapter.bluetoothLeScanner?.stopScan(scanCallback)
    }

    fun connect(item: ScanItem) {
        autoConnect = false
        prefs.edit().putString("last", item.address).apply()
        tracker.reset()
        stats.value = Stats()
        dfu.forget()
        client.connect(item.device, item.label)
    }

    fun disconnect() {
        autoConnect = false
        stopRecording()
        client.disconnect()
    }

    fun setLive(on: Boolean) {
        if (on) {
            tracker.reset()
            stats.value = Stats()
        }
        client.setLive(on)
    }

    private fun onFrame(now: Long, f: AccelFrame) {
        f.samples.forEach(ring::add)
        tracker.onFrame(f.seq)
        stats.update { Stats(it.samples + f.samples.size, tracker.lost) }
        if (rec.value.active) {
            write(csvLines(now, f))
            rec.update { it.copy(samples = it.samples + f.samples.size) }
        }
    }

    private fun onReconnect(now: Long) {
        tracker.resync()
        if (rec.value.active) write(csvReconnect(now))
    }

    private fun write(text: String) {
        io.execute { writer?.let { w -> runCatching { w.write(text); w.flush() }.onFailure(::recordingFailed) } }
    }

    private fun recordingFailed(e: Throwable) {
        handler.post {
            recError.value = "Recording failed: ${e.message ?: e.javaClass.simpleName}"
            stopRecording()
        }
    }

    fun startRecording() {
        if (rec.value.active) return
        val now = Date()
        val name = "gtfog_" + SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(now) + ".csv"
        recError.value = null
        // Opened on the io thread, before the first write queued after it; a full or missing storage ends here, not in a crash.
        io.execute {
            runCatching { writer = File(dir, name).bufferedWriter() }.onFailure(::recordingFailed)
        }
        val d = client.state.value
        val info = "app=GTFOg ${BuildConfig.VERSION_NAME}, device=${d.model ?: "?"} by ${d.manufacturer ?: "?"} ${d.address}, rate=100 Hz"
        write(csvPreamble(ZonedDateTime.now().format(DateTimeFormatter.ISO_OFFSET_DATE_TIME), info))
        rec.value = RecState(name, now.time, 0, 0, true)
        if (!d.live) setLive(true)
        // The service starts only once the file exists.
        io.execute { if (writer != null) handler.post { if (rec.value.active) ContextCompat.startForegroundService(app, Intent(app, RecordService::class.java)) } }
        refreshRecordings()
    }

    fun stopRecording() {
        if (!rec.value.active) return
        io.execute {
            runCatching { writer?.close() }
            writer = null
        }
        rec.update { it.copy(active = false, endMs = System.currentTimeMillis()) }
        refreshRecordings()
    }

    fun refreshRecordings() {
        recordings.value = dir.listFiles { f -> f.name.endsWith(".csv") }?.sortedByDescending { it.name } ?: emptyList()
    }

    fun delete(f: File) {
        f.delete()
        refreshRecordings()
    }
}

val StateFlow<RecState>.durationMs: Long
    get() = value.let { (if (it.active) System.currentTimeMillis() else it.endMs) - it.startMs }
