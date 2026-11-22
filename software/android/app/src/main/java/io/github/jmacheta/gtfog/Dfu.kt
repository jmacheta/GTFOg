package io.github.jmacheta.gtfog

import android.annotation.SuppressLint
import android.content.Context
import kotlinx.coroutines.flow.MutableStateFlow
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

/** The signed image from a raw `.bin` or from an NCS `dfu_application.zip` (its single `.bin` entry). */
fun loadImage(data: ByteArray): ByteArray {
    val bin = if (data.size > 4 && data[0] == 'P'.code.toByte() && data[1] == 'K'.code.toByte()) {
        val bins = ArrayList<ByteArray>()
        ZipInputStream(ByteArrayInputStream(data)).use { z ->
            while (true) {
                val e = z.nextEntry ?: break
                if (!e.isDirectory && e.name.endsWith(".bin", ignoreCase = true)) bins += z.readBytes()
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

data class DfuState(
    val installed: String? = null,
    val file: String? = null,
    val fileVersion: String? = null,
    val image: ByteArray? = null,
    val running: Boolean = false,
    val status: String = "",
    val percent: Int = 0,
    val error: String? = null,
    val done: Boolean = false,
)

/** Firmware update over mcumgr SMP (nRF Connect Device Manager library) on a second GATT link to the connected device. */
@SuppressLint("MissingPermission")
class Dfu(private val ctx: Context, private val client: GtfogClient) {
    private val _state = MutableStateFlow(DfuState())
    val state: StateFlow<DfuState> = _state
    private var manager: FirmwareUpgradeManager? = null

    /** Reads the version of slot 0 from the device. */
    fun refresh() {
        val device = client.currentDevice() ?: return
        if (_state.value.running) return
        val t = McuMgrBleTransport(ctx, device)
        ImageManager(t).list(object : McuMgrCallback<McuMgrImageStateResponse> {
            override fun onResponse(response: McuMgrImageStateResponse) {
                val v = response.images?.firstOrNull { it.image == 0 && it.slot == 0 }?.version
                _state.update { it.copy(installed = v) }
                t.release()
            }

            override fun onError(error: McuMgrException) {
                t.release()
            }
        })
    }

    fun pick(name: String, data: ByteArray?) {
        val r = runCatching { loadImage(requireNotNull(data) { "Cannot read the file" }) }
        _state.update {
            it.copy(
                file = name, image = r.getOrNull(), fileVersion = r.getOrNull()?.let(::parseImageVersion),
                error = r.exceptionOrNull()?.message, done = false,
            )
        }
    }

    fun install() {
        val image = _state.value.image ?: return
        val device = client.currentDevice() ?: return
        if (_state.value.running) return
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
                finish { it.copy(status = "Done", percent = 100, done = true, installed = it.fileVersion) }
                refresh()
            }

            override fun onUpgradeFailed(state: State, error: McuMgrException) =
                finish { it.copy(error = "Update failed while ${state.name.lowercase()}: ${error.message}") }

            override fun onUpgradeCanceled(state: State) = finish { it.copy(error = "Update canceled") }

            override fun onUploadProgressChanged(bytesSent: Int, imageSize: Int, timestamp: Long) {
                _state.update { it.copy(percent = (bytesSent * 100L / imageSize).toInt()) }
            }
        })
        m.setMode(FirmwareUpgradeManager.Mode.TEST_AND_CONFIRM)
        manager = m
        _state.update { it.copy(running = true, status = "Connecting", percent = 0, error = null, done = false) }
        runCatching {
            m.start(
                image,
                FirmwareUpgradeManager.Settings.Builder()
                    .setEstimatedSwapTime(20_000)
                    .setWindowCapacity(4)
                    .setMemoryAlignment(4)
                    .build(),
            )
        }.onFailure { e -> finish { it.copy(error = e.message ?: "Cannot start the update") } }
    }

    fun cancel() {
        manager?.cancel()
    }
}
