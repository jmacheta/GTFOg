package io.github.jmacheta.gtfog

import android.annotation.SuppressLint
import android.app.Application
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Intent
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

data class ScanItem(val device: BluetoothDevice, val name: String, val address: String, val rssi: Int)

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
    val adapter = app.getSystemService(BluetoothManager::class.java)?.adapter
    val scanResults = MutableStateFlow<List<ScanItem>>(emptyList())
    val stats = MutableStateFlow(Stats())
    val rec = MutableStateFlow(RecState())
    val recordings = MutableStateFlow<List<File>>(emptyList())

    private val tracker = SeqTracker()
    private val io = Executors.newSingleThreadExecutor()
    private var writer: BufferedWriter? = null
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
        }
    }

    fun startScan() {
        found.clear()
        scanResults.value = emptyList()
        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(Uuids.SERVICE)).build()
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        adapter?.bluetoothLeScanner?.startScan(listOf(filter), settings, scanCallback)
    }

    fun stopScan() {
        if (adapter?.isEnabled == true) adapter.bluetoothLeScanner?.stopScan(scanCallback)
    }

    fun connect(item: ScanItem) {
        tracker.reset()
        stats.value = Stats()
        client.connect(item.device, item.name)
    }

    fun disconnect() {
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
        val w = writer ?: return
        io.execute { runCatching { w.write(text); w.flush() } }
    }

    fun startRecording() {
        if (rec.value.active) return
        val now = Date()
        val name = "gtfog_" + SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(now) + ".csv"
        writer = File(dir, name).bufferedWriter()
        val d = client.state.value
        val info = "app=GTFOg ${BuildConfig.VERSION_NAME}, device=${d.model ?: "?"} by ${d.manufacturer ?: "?"} ${d.address}, rate=100 Hz"
        write(csvPreamble(ZonedDateTime.now().format(DateTimeFormatter.ISO_OFFSET_DATE_TIME), info))
        rec.value = RecState(name, now.time, 0, 0, true)
        if (!d.live) setLive(true)
        ContextCompat.startForegroundService(app, Intent(app, RecordService::class.java))
        refreshRecordings()
    }

    fun stopRecording() {
        if (!rec.value.active) return
        val w = writer
        writer = null
        io.execute { runCatching { w?.close() } }
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
