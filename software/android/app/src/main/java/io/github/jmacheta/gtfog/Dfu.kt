package io.github.jmacheta.gtfog

import android.annotation.SuppressLint
import android.content.Context
import android.os.SystemClock
import io.runtime.mcumgr.McuMgrCallback
import io.runtime.mcumgr.ble.McuMgrBleTransport
import io.runtime.mcumgr.dfu.FirmwareUpgradeCallback
import io.runtime.mcumgr.dfu.FirmwareUpgradeController
import io.runtime.mcumgr.dfu.mcuboot.FirmwareUpgradeManager
import io.runtime.mcumgr.dfu.mcuboot.FirmwareUpgradeManager.State
import io.runtime.mcumgr.exception.McuMgrException
import io.runtime.mcumgr.managers.ImageManager
import io.runtime.mcumgr.response.img.McuMgrImageStateResponse
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong
import java.util.zip.ZipInputStream

private const val IMAGE_MAGIC = 0x96f3b83dL
private const val STALL_MS = 90_000L // longer than the reset phase (swap time 20 s plus reconnect)
private const val UPDATE_FAILED =
    "Update failed. The goggles keep running the firmware they started with; check the version after reconnecting."

/** True when mcumgr has shown no sign of life for [STALL_MS] (the update is then failed by the watchdog). */
fun stalled(lastMs: Long, nowMs: Long, limitMs: Long = STALL_MS) = nowMs - lastMs >= limitMs

/** Returns once [stalled] is true for the clock values given; [wait] is the pause between looks (a fake clock in tests). */
suspend fun awaitStall(last: () -> Long, now: () -> Long, wait: suspend () -> Unit = { delay(5_000) }) {
    while (!stalled(last(), now())) wait()
}

/** MCUboot version from the image header (offset 20), formatted like mcumgr: major.minor.revision[+build]. */
fun parseImageVersion(b: ByteArray): String? {
    if (b.size < 24) return null
    fun u8(i: Int) = b[i].toInt() and 0xFF
    val magic = (u8(3).toLong() shl 24) or (u8(2).toLong() shl 16) or (u8(1).toLong() shl 8) or u8(0).toLong()
    if (magic != IMAGE_MAGIC) return null
    val build = (u8(27).toLong() shl 24) or (u8(26).toLong() shl 16) or (u8(25).toLong() shl 8) or u8(24).toLong()
    // ponytail: offset 20 = u8 major, u8 minor, u16 revision, u32 build
    val rev = (u8(23) shl 8) or u8(22)
    return "${u8(20)}.${u8(21)}.$rev" + if (build != 0L) "+$build" else ""
}

const val MAX_IMAGE = 4_000_000

/** Reads at most [max] bytes (a small zip can inflate far beyond its size). */
fun InputStream.readCapped(max: Int): ByteArray {
    val out = ByteArrayOutputStream()
    val buf = ByteArray(8192)
    while (true) {
        val n = read(buf)
        if (n < 0) return out.toByteArray()
        out.write(buf, 0, n)
        require(out.size() <= max) { "File too large (over 4 MB)" }
    }
}

/** The signed image from a raw `.bin` or from an NCS `dfu_application.zip` (its single `.bin` entry). */
fun loadImage(data: ByteArray): ByteArray {
    val bin = if (data.size > 4 && data[0] == 'P'.code.toByte() && data[1] == 'K'.code.toByte()) {
        val bins = ArrayList<ByteArray>()
        ZipInputStream(ByteArrayInputStream(data)).use { z ->
            while (true) {
                val e = z.nextEntry ?: break
                if (!e.isDirectory && e.name.endsWith(".bin", ignoreCase = true)) bins += z.readCapped(MAX_IMAGE)
            }
        }
        require(bins.size == 1) { if (bins.isEmpty()) "No .bin file in the zip" else "More than one .bin file in the zip" }
        bins[0]
    } else {
        data
    }
    require(parseImageVersion(bin) != null) { "Not an MCUboot image (bad header magic)" }
    return bin
}

// The device reports the build number as "0.3.8.1", the image header as "0.3.8+1".
private fun versionParts(v: String): List<Long>? =
    Regex("""(\d+)\.(\d+)\.(\d+)(?:[+.](\d+))?""").matchEntire(v)?.groupValues?.drop(1)?.map { it.toLongOrNull() ?: 0L }

/** Hint when the file is not newer than what runs on the device; null if newer or not comparable. */
fun versionHint(installed: String?, file: String?): String? {
    val a = versionParts(installed ?: return null) ?: return null
    val b = versionParts(file ?: return null) ?: return null
    for (i in 0 until 4) {
        val d = a[i].compareTo(b[i])
        if (d > 0) return "Older than the installed version"
        if (d < 0) return null
    }
    return "Same version is already installed"
}

/** Why Install must not start now: [dfuBlockReason], or a file that MCUboot's downgrade prevention would reject; null if allowed. */
fun installBlockReason(battery: Int?, installed: String?, file: String?): String? = dfuBlockReason(battery) ?: versionHint(installed, file)

/** Where an update is; [label] is the text shown. */
enum class DfuPhase(val label: String) {
    Idle(""), Connecting("Connecting"), Validating("Validating"), Uploading("Uploading"),
    Testing("Testing"), Resetting("Resetting"), Confirming("Confirming"), Done("Done"),
}

fun phaseOf(state: State) = when (state) {
    State.VALIDATE -> DfuPhase.Validating
    State.UPLOAD -> DfuPhase.Uploading
    State.TEST -> DfuPhase.Testing
    State.RESET -> DfuPhase.Resetting
    State.CONFIRM -> DfuPhase.Confirming
    State.NONE -> DfuPhase.Idle
}

data class DfuState(
    val installed: String? = null,
    val file: String? = null,
    val fileVersion: String? = null,
    val hasImage: Boolean = false, // the image bytes are kept in [Dfu], not in this data class
    val running: Boolean = false,
    val phase: DfuPhase = DfuPhase.Idle,
    val percent: Int = 0,
    val error: String? = null,
    val detail: String? = null,
    val done: Boolean = false,
    val refreshError: String? = null,
    val versionLost: Boolean = false, // the goggles did not come back after an update, so the version is unknown
)

/** Firmware update over mcumgr SMP (nRF Connect Device Manager library) on a second GATT link to the connected device. */
@SuppressLint("MissingPermission")
class Dfu(private val ctx: Context, private val client: GtfogClient, private val beforeInstall: () -> Unit = {}) {
    private val _state = MutableStateFlow(DfuState())
    val state: StateFlow<DfuState> = _state
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    // The mcumgr callbacks run on library threads; the rest runs on the main thread.
    @Volatile private var manager: FirmwareUpgradeManager? = null
    private val refreshing = AtomicBoolean(false)
    @Volatile private var image: ByteArray? = null

    /** A new connection: the installed version read earlier may belong to other goggles or an older firmware. */
    fun forget() = _state.update { it.copy(installed = null, refreshError = null, versionLost = false) }

    /** Reads the version of slot 0 from the device (one read at a time; the error is shown in the card). */
    fun refresh(attempt: Int = 0) {
        if (client.state.value.conn != Conn.Connected) return
        val device = client.currentDevice() ?: return
        if (_state.value.running || !refreshing.compareAndSet(false, true)) return
        val t = McuMgrBleTransport(ctx, device)
        val answered = AtomicBoolean(false) // per call: a late answer of a timed-out call must not end a newer one
        var timeout: Job? = null
        fun done(f: (DfuState) -> DfuState): Boolean {
            timeout?.cancel()
            if (!answered.compareAndSet(false, true)) return false // answered or timed out already
            refreshing.set(false)
            t.release()
            _state.update(f)
            return true
        }
        // A failed read is tried again twice (the first access may only trigger pairing or a slow link).
        fun fail(text: String) {
            if (done { it.copy(refreshError = text) } && attempt < 2) scope.launch {
                delay(3_000)
                if (client.state.value.conn == Conn.Connected && _state.value.installed == null) refresh(attempt + 1)
            }
        }
        timeout = scope.launch {
            delay(30_000)
            fail("no answer")
        }
        ImageManager(t).list(object : McuMgrCallback<McuMgrImageStateResponse> {
            override fun onResponse(response: McuMgrImageStateResponse) {
                val v = response.images?.firstOrNull { it.image == 0 && it.slot == 0 }?.version
                done { it.copy(installed = v, refreshError = if (v == null) "no image in slot 0" else null) }
            }

            override fun onError(error: McuMgrException) = fail(error.message ?: "error")
        })
    }

    /** [data] is the file; null with a [problem] text when it could not be read. */
    fun pick(name: String, data: ByteArray?, problem: String = "Cannot read the file") {
        val r = runCatching { loadImage(requireNotNull(data) { problem }) }
        image = r.getOrNull()
        _state.update {
            it.copy(
                file = name, hasImage = image != null, fileVersion = r.getOrNull()?.let(::parseImageVersion),
                error = r.exceptionOrNull()?.message, detail = null, done = false,
            )
        }
    }

    fun install() {
        val image = image ?: return
        val device = client.currentDevice() ?: return
        val s = _state.value
        if (s.running) return
        if (refreshing.get()) {
            _state.update { it.copy(error = "Still reading the installed version, try again in a moment", detail = null) }
            return
        }
        installBlockReason(client.state.value.battery, s.installed, s.fileVersion)?.let { r ->
            _state.update { it.copy(error = r, detail = null) }
            return
        }
        beforeInstall()
        val t = McuMgrBleTransport(ctx, device)
        val finished = AtomicBoolean(false) // the watchdog and the library callbacks may both end the update
        val last = AtomicLong(SystemClock.elapsedRealtime()) // last sign of life from mcumgr
        var watchdog: Job? = null
        fun finish(f: (DfuState) -> DfuState): Boolean {
            if (!finished.compareAndSet(false, true)) return false
            watchdog?.cancel()
            t.release()
            manager = null
            _state.update { f(it.copy(running = false)) }
            return true
        }
        val m = FirmwareUpgradeManager(t, object : FirmwareUpgradeCallback<State> {
            override fun onUpgradeStarted(controller: FirmwareUpgradeController) {}

            override fun onStateChanged(prevState: State, newState: State) {
                last.set(SystemClock.elapsedRealtime())
                _state.update { it.copy(phase = phaseOf(newState)) }
            }

            override fun onUpgradeCompleted() {
                // The file's version is not proof: wait for the goggles to be back, then ask them what runs.
                if (finish { it.copy(phase = DfuPhase.Done, percent = 100, done = true, installed = null) }) refreshWhenBack()
            }

            override fun onUpgradeFailed(state: State, error: McuMgrException) {
                val ended = finish {
                    it.copy(error = UPDATE_FAILED, detail = "Failed while ${state.name.lowercase()}: ${error.message}")
                }
                if (ended && (state == State.RESET || state == State.CONFIRM)) refreshWhenBack()
            }

            override fun onUpgradeCanceled(state: State) {
                finish { it.copy(error = "Update canceled") }
            }

            override fun onUploadProgressChanged(bytesSent: Int, imageSize: Int, timestamp: Long) {
                last.set(SystemClock.elapsedRealtime())
                _state.update { it.copy(percent = (bytesSent * 100L / imageSize).toInt()) }
            }
        })
        m.setMode(FirmwareUpgradeManager.Mode.TEST_AND_CONFIRM)
        manager = m
        // mcumgr may never call back (library hang, half-open link): do not lock the app for good.
        watchdog = scope.launch {
            awaitStall(last::get, SystemClock::elapsedRealtime)
            // Fail first: cancel() may call onUpgradeCanceled, which would end the update with "Update canceled" instead.
            if (finish { it.copy(error = UPDATE_FAILED, detail = "No answer from the goggles for ${STALL_MS / 1000} s") }) {
                runCatching { m.cancel() }
                refreshWhenBack()
            }
        }
        _state.update { it.copy(running = true, phase = DfuPhase.Connecting, percent = 0, error = null, detail = null, done = false) }
        runCatching {
            m.start(
                image,
                FirmwareUpgradeManager.Settings.Builder()
                    .setEstimatedSwapTime(20_000)
                    .setWindowCapacity(4)
                    .setMemoryAlignment(4)
                    .build(),
            )
        }.onFailure { e -> finish { it.copy(error = e.message ?: "Cannot start the update", detail = null) } }
    }

    /** Waits (at most 60 s) until the goggles are connected again, then reads the installed version. */
    private fun refreshWhenBack() {
        scope.launch {
            if (withTimeoutOrNull(60_000) { client.state.first { it.conn == Conn.Connected } } != null) {
                refresh()
            } else {
                _state.update { it.copy(versionLost = true) } // read again on the next connection (Settings tab, [forget])
            }
        }
    }

    fun cancel() {
        manager?.cancel()
    }
}
