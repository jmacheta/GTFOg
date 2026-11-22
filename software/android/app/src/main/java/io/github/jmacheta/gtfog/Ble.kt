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
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import java.util.UUID

/** Disconnected: the user still wants the link but it is down (Bluetooth off); [DeviceState.error] says why. */
enum class Conn { Idle, Connecting, Connected, Reconnecting, Disconnected }

private const val CONNECT_TIMEOUT_MS = 15_000L
private const val OP_TIMEOUT_MS = 5_000L

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
)

private class Op(val abandon: () -> Unit, val run: () -> Boolean) {
    var tries = 0
}

/**
 * GATT client for one GTFOg. Everything runs on the main thread and GATT operations go through a
 * serial queue; operations that fail for lack of encryption are retried while Android pairs. Connecting,
 * MTU and discovery must finish within 15 s and a queued operation without a callback is dropped after 5 s.
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
    private var live = false
    private var chars = emptyMap<UUID, BluetoothGattCharacteristic>()
    private val queue = ArrayDeque<Op>()
    private var cur: Op? = null

    // Writes queued or in flight per characteristic. While any exist, notifications and reads of that
    // characteristic are ignored: they carry older values and would overwrite the newer optimistic one.
    private val pendingWrites = HashMap<UUID, Int>()

    private val connectTimeout = Runnable { lost("Could not connect, try again") }

    // ponytail: a late callback of an abandoned operation completes the next one; the queue is cleared on every disconnect.
    private val opTimeout = object : Runnable {
        override fun run() {
            val op = cur ?: return
            // Pairing waits for the user: keep waiting while the bond is being created.
            if (device?.bondState == BluetoothDevice.BOND_BONDING) {
                handler.postDelayed(this, OP_TIMEOUT_MS)
                return
            }
            op.abandon()
            next()
        }
    }

    private val btReceiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) {
            when (i.getIntExtra(BluetoothAdapter.EXTRA_STATE, -1)) {
                BluetoothAdapter.STATE_TURNING_OFF, BluetoothAdapter.STATE_OFF -> btOff()
                BluetoothAdapter.STATE_ON -> if (_state.value.conn == Conn.Disconnected) reconnect()
            }
        }
    }

    init {
        // ponytail: never unregistered, the client lives as long as the process.
        ContextCompat.registerReceiver(ctx, btReceiver, IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED), ContextCompat.RECEIVER_NOT_EXPORTED)
    }

    fun connect(d: BluetoothDevice, name: String) {
        disconnect()
        device = d
        wantConnected = true
        everConnected = false
        _state.value = DeviceState(Conn.Connecting, name, d.address)
        open(false)
    }

    fun currentDevice() = device

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
        clearQueue()
        gatt?.close()
        gatt = null
    }

    /** The link is gone or never came up: retry in the background if it had worked before, else report. */
    private fun lost(message: String) {
        teardown()
        if (wantConnected && everConnected) {
            _state.update { it.copy(conn = Conn.Reconnecting, error = null) }
            open(true)
        } else {
            wantConnected = false
            _state.value = DeviceState(error = message)
        }
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
        handler.removeCallbacks(opTimeout)
        queue.clear()
        cur = null
        pendingWrites.clear()
    }

    private fun enqueue(abandon: () -> Unit = {}, run: () -> Boolean) {
        queue.addLast(Op(abandon, run))
        pump()
    }

    private fun start(op: Op): Boolean {
        cur = op
        handler.removeCallbacks(opTimeout)
        handler.postDelayed(opTimeout, OP_TIMEOUT_MS)
        return op.run()
    }

    private fun pump() {
        while (cur == null) {
            val op = queue.removeFirstOrNull() ?: return
            if (!start(op)) cur = null
        }
    }

    private fun next() {
        handler.removeCallbacks(opTimeout)
        cur = null
        pump()
    }

    private fun finish(status: Int) {
        val op = cur ?: return
        if ((status == 5 || status == 15) && op.tries < 6) {
            op.tries++
            handler.postDelayed({ if (cur === op && !start(op)) next() }, 1500)
        } else {
            next()
        }
    }

    private fun read(u: UUID) {
        val c = chars[u] ?: return
        enqueue { gatt?.readCharacteristic(c) == true }
    }

    private fun write(u: UUID, v: Int) = write(u, byteArrayOf(v.toByte()))

    private fun write(u: UUID, bytes: ByteArray) {
        val c = chars[u] ?: return
        pendingWrites[u] = (pendingWrites[u] ?: 0) + 1
        enqueue({ writeDone(u) }) {
            c.value = bytes
            c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            val ok = gatt?.writeCharacteristic(c) == true
            if (!ok) writeDone(u)
            ok
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
        enqueue {
            d.value = if (on) android.bluetooth.BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
            else android.bluetooth.BluetoothGattDescriptor.DISABLE_NOTIFICATION_VALUE
            gatt?.writeDescriptor(d) == true
        }
    }

    private fun setup() {
        handler.removeCallbacks(connectTimeout)
        val reconnected = everConnected
        everConnected = true
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
                // Connected (also after autoConnect): MTU and discovery must follow within the deadline.
                handler.removeCallbacks(connectTimeout)
                handler.postDelayed(connectTimeout, CONNECT_TIMEOUT_MS)
                if (!g.requestMtu(247)) g.discoverServices()
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                lost(gattErrorText(status))
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            if (g === gatt) g.discoverServices()
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
            finish(status)
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
            if (g !== gatt) return
            val retry = (status == 5 || status == 15) && (cur?.tries ?: 6) < 6
            if (!retry) writeDone(c.uuid)
            if (status != 0 && status != 5 && status != 15) read(c.uuid) // the device rejected it: show its value
            finish(status)
        }

        override fun onDescriptorWrite(g: BluetoothGatt, d: android.bluetooth.BluetoothGattDescriptor, status: Int) {
            if (g === gatt) finish(status)
        }

        // API 33+ passes the value; the older callbacks read it from the shared characteristic object.
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray) {
            if (g === gatt) onValue(c.uuid, value)
        }

        override fun onCharacteristicRead(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray, status: Int) {
            if (g !== gatt) return
            if (status == 0) onValue(c.uuid, value)
            finish(status)
        }

        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            if (g === gatt) onValue(c.uuid, c.value ?: ByteArray(0))
        }
    }
}
