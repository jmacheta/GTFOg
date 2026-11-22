package io.github.jmacheta.gtfog

import android.annotation.SuppressLint
import android.content.Context
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import io.runtime.mcumgr.McuMgrCallback
import io.runtime.mcumgr.ble.McuMgrBleTransport
import io.runtime.mcumgr.dfu.FirmwareUpgradeCallback
import io.runtime.mcumgr.dfu.FirmwareUpgradeController
import io.runtime.mcumgr.dfu.mcuboot.FirmwareUpgradeManager
import io.runtime.mcumgr.dfu.mcuboot.FirmwareUpgradeManager.State
import io.runtime.mcumgr.exception.McuMgrException
import io.runtime.mcumgr.managers.ImageManager
import io.runtime.mcumgr.response.img.McuMgrImageStateResponse
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.util.concurrent.atomic.AtomicBoolean
import java.util.zip.ZipInputStream

private const val IMAGE_MAGIC = 0x96f3b83dL

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

private fun versionParts(v: String): List<Long>? =
    Regex("""(\d+)\.(\d+)\.(\d+)(?:\+(\d+))?""").matchEntire(v)?.groupValues?.drop(1)?.map { it.toLongOrNull() ?: 0L }

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

data class DfuState(
    val installed: String? = null,
    val file: String? = null,
    val fileVersion: String? = null,
    val image: ByteArray? = null,
    val running: Boolean = false,
    val status: String = "",
    val percent: Int = 0,
    val error: String? = null,
    val detail: String? = null,
    val done: Boolean = false,
    val refreshError: String? = null,
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

    /** A new connection: the installed version read earlier may belong to other goggles or an older firmware. */
    fun forget() = _state.update { it.copy(installed = null, refreshError = null) }

    /** Reads the version of slot 0 from the device (one read at a time; the error is shown in the card). */
    fun refresh(attempt: Int = 0) {
        val device = client.currentDevice() ?: return
        if (_state.value.running || !refreshing.compareAndSet(false, true)) return
        val t = McuMgrBleTransport(ctx, device)
        val answered = AtomicBoolean(false) // per call: a late answer of a timed-out call must not end a newer one
        var timeout: Job? = null
        fun done(f: (DfuState) -> DfuState) {
            timeout?.cancel()
            if (!answered.compareAndSet(false, true)) return // answered or timed out already
            refreshing.set(false)
            t.release()
            _state.update(f)
        }
        // A failed read is tried again twice (the first access may only trigger pairing or a slow link).
        fun fail(text: String) {
            done { it.copy(refreshError = text) }
            if (attempt < 2) scope.launch {
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
        _state.update {
            it.copy(
                file = name, image = r.getOrNull(), fileVersion = r.getOrNull()?.let(::parseImageVersion),
                error = r.exceptionOrNull()?.message, detail = null, done = false,
            )
        }
    }

    fun install() {
        val image = _state.value.image ?: return
        val device = client.currentDevice() ?: return
        if (_state.value.running) return
        if (refreshing.get()) { _state.update { it.copy(error = "Still reading the installed version, try again in a moment", detail = null) }; return }
        dfuBlockReason(client.state.value.battery)?.let { r -> _state.update { it.copy(error = r, detail = null) }; return }
        beforeInstall()
        val t = McuMgrBleTransport(ctx, device)
        fun finish(f: (DfuState) -> DfuState) {
            t.release()
            manager = null
            _state.update { f(it.copy(running = false)) }
        }
        val m = FirmwareUpgradeManager(t, object : FirmwareUpgradeCallback<State> {
            override fun onUpgradeStarted(controller: FirmwareUpgradeController) {}

            override fun onStateChanged(prevState: State, newState: State) {
                val text = when (newState) {
                    State.VALIDATE -> "Validating"
                    State.UPLOAD -> "Uploading"
                    State.TEST -> "Testing"
                    State.RESET -> "Resetting"
                    State.CONFIRM -> "Confirming"
                    State.NONE -> ""
                }
                _state.update { it.copy(status = text) }
            }

            override fun onUpgradeCompleted() {
                // The file's version is not proof: wait for the goggles to be back, then ask them what runs.
                finish { it.copy(status = "Done", percent = 100, done = true, installed = null) }
                refreshWhenBack()
            }

            override fun onUpgradeFailed(state: State, error: McuMgrException) = finish {
                it.copy(
                    error = "Update failed. The goggles keep running the firmware they started with; check the version after reconnecting.",
                    detail = "Failed while ${state.name.lowercase()}: ${error.message}",
                )
            }.also { if (state == State.RESET || state == State.CONFIRM) refreshWhenBack() }

            override fun onUpgradeCanceled(state: State) = finish { it.copy(error = "Update canceled") }

            override fun onUploadProgressChanged(bytesSent: Int, imageSize: Int, timestamp: Long) {
                _state.update { it.copy(percent = (bytesSent * 100L / imageSize).toInt()) }
            }
        })
        m.setMode(FirmwareUpgradeManager.Mode.TEST_AND_CONFIRM)
        manager = m
        _state.update { it.copy(running = true, status = "Connecting", percent = 0, error = null, detail = null, done = false) }
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
            withTimeoutOrNull(60_000) { client.state.first { it.conn == Conn.Connected } }
            refresh()
        }
    }

    fun cancel() {
        manager?.cancel()
    }
}
