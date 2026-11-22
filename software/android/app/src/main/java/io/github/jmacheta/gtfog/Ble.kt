package io.github.jmacheta.gtfog

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Build
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
private const val CLOSE_TIMEOUT_MS = 1_000L

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
    /** Notifications could not be enabled (after the retries); shown in the Debug tab. */
    val streamError: String? = null,
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
    private var up = false // GATT link is up but the services are not ready yet
    private val policy = LinkPolicy()
    private var live = false
    private var chars = emptyMap<UUID, BluetoothGattCharacteristic>()
    private val timer = object : Timer {
        override fun postDelayed(r: Runnable, ms: Long) {
            handler.postDelayed(r, ms)
        }

        override fun remove(r: Runnable) = handler.removeCallbacks(r)
    }
    private val ops = OpQueue(timer, { device?.bondState == BluetoothDevice.BOND_BONDING }, { authFailed(isBonded()) }, { lost("Connection stalled") })
    private val backoff = Backoff()

    private val pendingWrites = PendingWrites<UUID>()

    private val connectTimeout = Runnable { lost("Could not connect, try again") }

    private var closing: BluetoothGatt? = null
    private val closeTimeout = Runnable { closeNow() }

    private val reopen = Runnable { if (wantConnected && gatt == null) open(policy.everConnected) }

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
            // BONDED -> NONE: the phone had a bond that is gone; BONDING -> NONE: a new pairing was cancelled or refused.
            bondLost(i.getIntExtra(BluetoothDevice.EXTRA_PREVIOUS_BOND_STATE, -1), i.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, -1))
                ?.let(::authFailed)
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
        policy.reset()
        backoff.reset()
        _state.value = DeviceState(Conn.Connecting, name, d.address)
        open(false)
    }

    fun currentDevice() = device

    fun clearNotice() = _state.update { it.copy(notice = null) }

    fun disconnect() {
        wantConnected = false
        live = false
        hangUp()
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
        _state.update { it.copy(conn = if (policy.everConnected) Conn.Reconnecting else Conn.Connecting, error = null) }
        open(false)
    }

    private fun btOff() {
        if (!wantConnected || _state.value.conn == Conn.Disconnected) return
        teardown()
        _state.update { it.copy(conn = Conn.Disconnected, error = "Bluetooth is off") }
    }

    /** Ends the link on purpose: disconnect now, close when Android confirms (or after 1 s), so the goggles see a clean LL terminate. */
    private fun hangUp() {
        closeNow()
        val g = gatt
        if (g != null) {
            g.disconnect()
            closing = g
            handler.postDelayed(closeTimeout, CLOSE_TIMEOUT_MS)
        }
        gatt = null // the callbacks of g are ignored from here on, except its DISCONNECTED
        teardown()
    }

    private fun closeNow() {
        handler.removeCallbacks(closeTimeout)
        closing?.close()
        closing = null
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
    private fun lost(message: String, next: Next? = null) {
        teardown()
        act(if (wantConnected) next ?: policy.onLost() else Next.GiveUp, message)
    }

    private fun act(next: Next, message: String) {
        _state.update { it.afterLost(next, message) }
        if (keepsTrying(next)) handler.postDelayed(reopen, backoff.next()) else wantConnected = false
    }

    /**
     * Authentication or encryption keeps failing: stop and tell the user what to do. With [hadBond] the goggles lost
     * the phone's key (remove the bond); without, a new pairing was refused or cancelled.
     */
    private fun authFailed(hadBond: Boolean) {
        if (!wantConnected) return
        wantConnected = false
        live = false
        hangUp()
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
        setFan(stepFan(st.fan ?: return, dir, st.settings?.fanStep ?: 5))
    }

    fun setLive(on: Boolean) {
        live = on
        _state.update { it.copy(live = on, streamError = if (on) null else it.streamError) }
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
        pendingWrites.begin(u)
        enqueue(u, { status ->
            // Read back what the device really has (it may clamp or adjust the value); after the last write of a burst only.
            if (pendingWrites.end(u)) {
                read(u)
                if (u == Uuids.FAN) read(Uuids.FAN_ACTUAL)
            }
            if (status != 0) _state.update { it.copy(notice = "Could not change ${charName(u)}", noticeSeq = it.noticeSeq + 1) }
        }) {
            writeCharacteristic(c, bytes)
        }
    }

    // API 33 replaced the shared c.value / d.value with explicit arguments; the old path serves API 26-32.
    @Suppress("DEPRECATION")
    private fun writeCharacteristic(c: BluetoothGattCharacteristic, bytes: ByteArray): Boolean {
        val g = gatt ?: return false
        if (Build.VERSION.SDK_INT >= 33) {
            return g.writeCharacteristic(c, bytes, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) == BluetoothStatusCodes.SUCCESS
        }
        c.value = bytes
        c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        return g.writeCharacteristic(c)
    }

    @Suppress("DEPRECATION")
    private fun writeDescriptor(d: BluetoothGattDescriptor, value: ByteArray): Boolean {
        val g = gatt ?: return false
        if (Build.VERSION.SDK_INT >= 33) return g.writeDescriptor(d, value) == BluetoothStatusCodes.SUCCESS
        d.value = value
        return g.writeDescriptor(d)
    }

    private fun notify(u: UUID, on: Boolean) {
        val c = chars[u] ?: return
        gatt?.setCharacteristicNotification(c, on)
        val d = c.getDescriptor(Uuids.CCCD) ?: return
        enqueue(cccdKey(u), { status -> if (status != 0 && on) subscribeFailed(u) }) {
            writeDescriptor(d, if (on) BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE else BluetoothGattDescriptor.DISABLE_NOTIFICATION_VALUE)
        }
    }

    private fun subscribeFailed(u: UUID) {
        if (u == Uuids.ACCEL) live = false // no stream will come: Live goes back off
        val text = if (u == Uuids.ACCEL) "Could not start the accelerometer stream" else "Could not enable notifications; values may not update"
        _state.update { it.copy(live = it.live && u != Uuids.ACCEL, streamError = text) }
    }

    private fun cccdKey(u: UUID) = "cccd $u"

    private fun setup() {
        handler.removeCallbacks(connectTimeout)
        val reconnected = policy.everConnected
        policy.ready()
        up = false
        backoff.reset()
        _state.update { it.copy(conn = Conn.Connected, error = null, streamError = null) }
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
        _state.update { it.withValue(u, v, pendingWrites.has(u)) }
    }

    private val callback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (g === closing) {
                if (newState == BluetoothProfile.STATE_DISCONNECTED) closeNow()
                return
            }
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
                val bonded = isBonded()
                val next = policy.onDisconnect(status, up, bonded)
                if (next == Next.AuthFailed) authFailed(bonded) else lost(gattErrorText(status), next)
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
            when (val next = policy.onDiscovered(status, Uuids.FAN in chars && Uuids.ACCEL in chars)) {
                null -> setup()
                // A discovery error is a transient link problem; only a successful discovery without the service is the wrong device.
                Next.NotGtfog -> {
                    disconnect()
                    _state.value = DeviceState(error = "Not a GTFOg (the GTFOg service was not found)")
                }
                else -> lost("Service discovery failed (status $status)", next)
            }
        }

        @Suppress("DEPRECATION")
        override fun onCharacteristicRead(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
            if (g !== gatt) return
            if (status == 0) onValue(c.uuid, c.value ?: ByteArray(0))
            ops.finish(status, c.uuid)
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
            if (g !== gatt) return
            ops.finish(status, c.uuid)
        }

        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
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

        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            if (g === gatt) onValue(c.uuid, c.value ?: ByteArray(0))
        }
    }
}
