package io.github.jmacheta.gtfog

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Handler
import android.os.Looper
import androidx.core.content.ContextCompat
import androidx.core.content.IntentCompat
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import java.util.UUID

/** Disconnected: the user still wants the link but it is down (Bluetooth off); [DeviceState.error] says why. */
enum class Conn { Idle, Connecting, Connected, Reconnecting, Disconnected }

private const val CONNECT_TIMEOUT_MS = 15_000L
private const val FIRST_CONNECT_RETRIES = 3 // status 133 on the first direct connect is a common Android flake
private const val EARLY_DROPS = 3 // bonded phone, link dropped before the services were found

const val PAIR_MESSAGE =
    "Pairing was refused. Turn the goggles off and on (pairing is open for 3 minutes after power-on), then connect again"

const val BOND_MESSAGE = "The goggles forgot this phone. Remove GTFOg in Android Bluetooth settings, then connect again"

data class DeviceState(
    val conn: Conn = Conn.Idle,
    val name: String = "",
    val address: String = "",
    val error: String? = null,
    val battery: Int? = null,
    val fan: Int? = null,
    val fanActual: Int? = null,
    val activity: Int? = null,
    val strobe: Boolean? = null,
    val charger: Int? = null,
    val settings: DeviceSettings? = null,
    val hasSettings: Boolean = true,
    val model: String? = null,
    val manufacturer: String? = null,
    val live: Boolean = false,
    /** Negotiated ATT MTU; null until known. A full accelerometer frame needs 247. */
    val mtu: Int? = null,
    /** Short message about a failed write; the UI shows it once and calls [GtfogClient.clearNotice]. */
    val notice: String? = null,
    /** Counts notices, so the UI can tell a repeated identical [notice] from the old one. */
    val noticeSeq: Int = 0,
    /** The bond with the goggles is bad: [error] says what to do, the UI offers the Bluetooth settings. */
    val bondProblem: Boolean = false,
)

private fun charName(u: UUID) = when (u) {
    Uuids.FAN -> "fan speed"
    Uuids.STROBE -> "strobe"
    Uuids.SETTINGS -> "the setting"
    else -> "the value"
}

/**
 * GATT client for one GTFOg. Everything runs on the main thread and GATT operations go through a
 * serial queue ([OpQueue]); operations that fail for lack of encryption are retried while Android pairs. Connecting,
 * MTU and discovery must finish within 15 s and a queued operation without a callback is dropped after 5 s.
 * A write that finally fails is read back and reported in [DeviceState.notice]; a bad bond in [DeviceState.bondProblem].
 * The client follows the Bluetooth adapter: off closes the link, on reconnects if the user still wants it.
 */
@SuppressLint("MissingPermission")
@Suppress("DEPRECATION")
class GtfogClient(
    private val ctx: Context,
    private val onFrame: (Long, AccelFrame) -> Unit,
    private val onReconnect: (Long) -> Unit,
) {
    private val handler = Handler(Looper.getMainLooper())
    private val _state = MutableStateFlow(DeviceState())
    val state: StateFlow<DeviceState> = _state.asStateFlow()

    private var gatt: BluetoothGatt? = null
    private var device: BluetoothDevice? = null
    private var wantConnected = false
    private var everConnected = false
    private var up = false // GATT link is up but the services are not ready yet
    private var firstFails = 0
    private var earlyDrops = 0
    private var live = false
    private var chars = emptyMap<UUID, BluetoothGattCharacteristic>()
    private val timer = object : Timer {
        override fun postDelayed(r: Runnable, ms: Long) {
            handler.postDelayed(r, ms)
        }

        override fun remove(r: Runnable) = handler.removeCallbacks(r)
    }
    private val ops = OpQueue(timer, { device?.bondState == BluetoothDevice.BOND_BONDING }, { authFailed(isBonded()) })
    private val backoff = Backoff()

    // Writes queued or in flight per characteristic. While any exist, notifications and reads of that
    // characteristic are ignored: they carry older values and would overwrite the newer optimistic one.
    private val pendingWrites = HashMap<UUID, Int>()

    private val connectTimeout = Runnable { lost("Could not connect, try again") }

    private val reopen = Runnable { if (wantConnected && gatt == null) open(everConnected) }

    private fun isBonded() = device?.bondState == BluetoothDevice.BOND_BONDED

    private val btReceiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) {
            when (i.getIntExtra(BluetoothAdapter.EXTRA_STATE, -1)) {
                BluetoothAdapter.STATE_TURNING_OFF, BluetoothAdapter.STATE_OFF -> btOff()
                BluetoothAdapter.STATE_ON -> if (_state.value.conn == Conn.Disconnected) reconnect()
            }
        }
    }

    // A bond that was refused, or removed while we use it, cannot be repaired by retrying.
    private val bondReceiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) {
            val d = IntentCompat.getParcelableExtra(i, BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
            if (d?.address != device?.address) return
            val prev = i.getIntExtra(BluetoothDevice.EXTRA_PREVIOUS_BOND_STATE, -1)
            // BONDED -> NONE: the phone had a bond that is gone; BONDING -> NONE: a new pairing was cancelled or refused.
            if (i.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, -1) == BluetoothDevice.BOND_NONE && prev != BluetoothDevice.BOND_NONE) {
                authFailed(prev == BluetoothDevice.BOND_BONDED)
            }
        }
    }

    init {
        // ponytail: never unregistered, the client lives as long as the process.
        ContextCompat.registerReceiver(ctx, btReceiver, IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED), ContextCompat.RECEIVER_NOT_EXPORTED)
        ContextCompat.registerReceiver(ctx, bondReceiver, IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED), ContextCompat.RECEIVER_NOT_EXPORTED)
    }

    fun connect(d: BluetoothDevice, name: String) {
        disconnect()
        device = d
        wantConnected = true
        everConnected = false
        firstFails = 0
        earlyDrops = 0
        backoff.reset()
        _state.value = DeviceState(Conn.Connecting, name, d.address)
        open(false)
    }

    fun currentDevice() = device

    fun clearNotice() = _state.update { it.copy(notice = null) }

    fun disconnect() {
        wantConnected = false
        live = false
        gatt?.disconnect()
        teardown()
        _state.value = DeviceState()
    }

    /** Opens a new direct connection to the same goggles (the Reconnect button, and Bluetooth turning on). */
    fun reconnect() {
        if (!wantConnected || device == null) return
        teardown()
        if (ctx.getSystemService(BluetoothManager::class.java)?.adapter?.isEnabled != true) {
            _state.update { it.copy(conn = Conn.Disconnected, error = "Bluetooth is off") }
            return
        }
        _state.update { it.copy(conn = if (everConnected) Conn.Reconnecting else Conn.Connecting, error = null) }
        open(false)
    }

    private fun btOff() {
        if (!wantConnected || _state.value.conn == Conn.Disconnected) return
        teardown()
        _state.update { it.copy(conn = Conn.Disconnected, error = "Bluetooth is off") }
    }

    private fun teardown() {
        handler.removeCallbacks(connectTimeout)
        handler.removeCallbacks(reopen)
        clearQueue()
        up = false
        gatt?.close()
        gatt = null
    }

    /** The link is gone or never came up: retry in the background if it had worked before, else report. */
    private fun lost(message: String, retryFirst: Boolean = false) {
        teardown()
        if (wantConnected && everConnected) {
            _state.update { it.copy(conn = Conn.Reconnecting, error = null) }
            handler.postDelayed(reopen, backoff.next())
        } else if (wantConnected && retryFirst && firstFails++ < FIRST_CONNECT_RETRIES) {
            handler.postDelayed(reopen, backoff.next()) // still "Connecting"
        } else {
            wantConnected = false
            _state.value = DeviceState(error = message)
        }
    }

    /**
     * Authentication or encryption keeps failing: stop and tell the user what to do. With [hadBond] the goggles lost
     * the phone's key (remove the bond); without, a new pairing was refused or cancelled.
     */
    private fun authFailed(hadBond: Boolean) {
        if (!wantConnected) return
        wantConnected = false
        live = false
        gatt?.disconnect()
        teardown()
        _state.value = DeviceState(error = if (hadBond) BOND_MESSAGE else PAIR_MESSAGE, bondProblem = hadBond)
    }

    fun setFan(percent: Int) {
        val v = percent.coerceIn(0, 100)
        _state.update { it.copy(fan = v, fanActual = v) }
        write(Uuids.FAN, v)
    }

    fun setStrobe(on: Boolean) {
        _state.update { it.copy(strobe = on) }
        write(Uuids.STROBE, if (on) 1 else 0)
    }

    /** Changes the settings from the latest state, not from a value captured by the UI. */
    fun updateSettings(f: (DeviceSettings) -> DeviceSettings) {
        val v = f(_state.value.settings ?: return)
        _state.update { it.copy(settings = v) }
        write(Uuids.SETTINGS, v.encode())
    }

    /** Fan up or down by the device's step per click (5 % for old firmware). */
    fun stepFan(dir: Int) {
        val st = _state.value
        setFan((st.fan ?: return) + dir * (st.settings?.fanStep ?: 5))
    }

    fun setLive(on: Boolean) {
        live = on
        _state.update { it.copy(live = on) }
        if (_state.value.conn == Conn.Connected) notify(Uuids.ACCEL, on)
    }

    private fun open(auto: Boolean) {
        // Reconnects use autoConnect so Android keeps trying while the phone is in a pocket.
        gatt = device?.connectGatt(ctx, auto, callback, BluetoothDevice.TRANSPORT_LE, BluetoothDevice.PHY_LE_1M_MASK, handler)
        // autoConnect waits as long as the goggles are away, so only a direct connect has a deadline.
        if (!auto) handler.postDelayed(connectTimeout, CONNECT_TIMEOUT_MS)
    }

    private fun clearQueue() {
        ops.clear()
        pendingWrites.clear()
    }

    private fun enqueue(key: Any? = null, onEnd: (Int) -> Unit = {}, run: () -> Boolean) = ops.enqueue(Op(run, onEnd, key))

    private fun read(u: UUID) {
        val c = chars[u] ?: return
        enqueue(key = u) { gatt?.readCharacteristic(c) == true }
    }

    private fun write(u: UUID, v: Int) = write(u, byteArrayOf(v.toByte()))

    private fun write(u: UUID, bytes: ByteArray) {
        val c = chars[u] ?: return
        pendingWrites[u] = (pendingWrites[u] ?: 0) + 1
        enqueue(u, { status ->
            writeDone(u)
            // Read back what the device really has (it may clamp or adjust the value); after the last write of a burst only.
            if (u !in pendingWrites) {
                read(u)
                if (u == Uuids.FAN) read(Uuids.FAN_ACTUAL)
            }
            if (status != 0) _state.update { it.copy(notice = "Could not change ${charName(u)}", noticeSeq = it.noticeSeq + 1) }
        }) {
            c.value = bytes
            c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            gatt?.writeCharacteristic(c) == true
        }
    }

    private fun writeDone(u: UUID) {
        val n = (pendingWrites[u] ?: 0) - 1
        if (n > 0) pendingWrites[u] = n else pendingWrites.remove(u)
    }

    private fun notify(u: UUID, on: Boolean) {
        val c = chars[u] ?: return
        gatt?.setCharacteristicNotification(c, on)
        val d = c.getDescriptor(Uuids.CCCD) ?: return
        enqueue(cccdKey(u)) {
            d.value = if (on) android.bluetooth.BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
            else android.bluetooth.BluetoothGattDescriptor.DISABLE_NOTIFICATION_VALUE
            gatt?.writeDescriptor(d) == true
        }
    }

    private fun cccdKey(u: UUID) = "cccd $u"

    private fun setup() {
        handler.removeCallbacks(connectTimeout)
        val reconnected = everConnected
        everConnected = true
        up = false
        earlyDrops = 0
        backoff.reset()
        _state.update { it.copy(conn = Conn.Connected, error = null) }
        listOf(Uuids.FAN, Uuids.FAN_ACTUAL, Uuids.STROBE, Uuids.CHARGER, Uuids.BATTERY, Uuids.SETTINGS).forEach { notify(it, true) }
        if (live) notify(Uuids.ACCEL, true)
        _state.update { it.copy(hasSettings = Uuids.SETTINGS in chars) }
        listOf(Uuids.BATTERY, Uuids.FAN, Uuids.FAN_ACTUAL, Uuids.STROBE, Uuids.CHARGER, Uuids.SETTINGS, Uuids.MODEL, Uuids.MANUFACTURER).forEach(::read)
        if (reconnected) onReconnect(System.currentTimeMillis())
    }

    private fun onValue(u: UUID, v: ByteArray) {
        if (u == Uuids.ACCEL) {
            parseAccelFrame(v)?.let { onFrame(System.currentTimeMillis(), it) }
            return
        }
        if (pendingWrites.containsKey(u)) return
        if (u == Uuids.SETTINGS) {
            parseSettings(v)?.let { s -> _state.update { it.copy(settings = s) } }
            return
        }
        if (v.isEmpty()) return
        val b = v[0].toInt() and 0xFF
        _state.update {
            when (u) {
                Uuids.FAN -> it.copy(fan = b)
                Uuids.FAN_ACTUAL -> parseFanActual(v)?.let { a -> it.copy(fanActual = a.percent, activity = a.level) } ?: it
                Uuids.STROBE -> it.copy(strobe = b != 0)
                Uuids.CHARGER -> it.copy(charger = b)
                Uuids.BATTERY -> it.copy(battery = b)
                Uuids.MODEL -> it.copy(model = v.decodeToString())
                Uuids.MANUFACTURER -> it.copy(manufacturer = v.decodeToString())
                else -> it
            }
        }
    }

    private val callback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (g !== gatt) return
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                clearQueue()
                up = true
                // Connected (also after autoConnect): MTU and discovery must follow within the deadline.
                handler.removeCallbacks(connectTimeout)
                handler.postDelayed(connectTimeout, CONNECT_TIMEOUT_MS)
                _state.update { it.copy(mtu = null) }
                if (!g.requestMtu(247)) {
                    _state.update { it.copy(mtu = 23) }
                    g.discoverServices()
                }
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                // A bonded phone whose link keeps dropping before the services are found: the goggles most likely lost the key.
                val early = up && isBonded() && ++earlyDrops >= EARLY_DROPS
                if (isAuthFailure(status) || early) authFailed(isBonded()) else lost(gattErrorText(status), retryFirst = status == 133)
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            if (g !== gatt) return
            _state.update { it.copy(mtu = if (status == 0) mtu else 23) }
            g.discoverServices()
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            if (g !== gatt) return
            chars = g.services.flatMap { it.characteristics }.associateBy { it.uuid }
            if (status != 0 || Uuids.FAN !in chars || Uuids.ACCEL !in chars) {
                disconnect()
                _state.value = DeviceState(error = "Not a GTFOg (service discovery status $status)")
            } else {
                setup()
            }
        }

        override fun onCharacteristicRead(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
            if (g !== gatt) return
            if (status == 0) onValue(c.uuid, c.value ?: ByteArray(0))
            ops.finish(status, c.uuid)
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
            if (g !== gatt) return
            ops.finish(status, c.uuid)
        }

        override fun onDescriptorWrite(g: BluetoothGatt, d: android.bluetooth.BluetoothGattDescriptor, status: Int) {
            if (g === gatt) ops.finish(status, cccdKey(d.characteristic.uuid))
        }

        // API 33+ passes the value; the older callbacks read it from the shared characteristic object.
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray) {
            if (g === gatt) onValue(c.uuid, value)
        }

        override fun onCharacteristicRead(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray, status: Int) {
            if (g !== gatt) return
            if (status == 0) onValue(c.uuid, value)
            ops.finish(status, c.uuid)
        }

        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            if (g === gatt) onValue(c.uuid, c.value ?: ByteArray(0))
        }
    }
}
