package io.github.jmacheta.gtfog

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothProfile
import android.content.Context
import android.os.Handler
import android.os.Looper
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import java.util.UUID

enum class Conn { Idle, Connecting, Connected, Reconnecting }

data class DeviceState(
    val conn: Conn = Conn.Idle,
    val name: String = "",
    val address: String = "",
    val error: String? = null,
    val battery: Int? = null,
    val fan: Int? = null,
    val strobe: Boolean? = null,
    val charger: Int? = null,
    val model: String? = null,
    val manufacturer: String? = null,
    val live: Boolean = false,
)

private class Op(val run: () -> Boolean) {
    var tries = 0
}

/**
 * GATT client for one GTFOg. Everything runs on the main thread and GATT operations go through a
 * serial queue; operations that fail for lack of encryption are retried while Android pairs.
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

    fun connect(d: BluetoothDevice, name: String) {
        disconnect()
        device = d
        wantConnected = true
        everConnected = false
        _state.value = DeviceState(Conn.Connecting, name, d.address)
        open(false)
    }

    fun disconnect() {
        wantConnected = false
        live = false
        clearQueue()
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        _state.value = DeviceState()
    }

    fun setFan(percent: Int) {
        val v = percent.coerceIn(0, 100)
        _state.update { it.copy(fan = v) }
        write(Uuids.FAN, v)
    }

    fun setStrobe(on: Boolean) {
        _state.update { it.copy(strobe = on) }
        write(Uuids.STROBE, if (on) 1 else 0)
    }

    fun setLive(on: Boolean) {
        live = on
        _state.update { it.copy(live = on) }
        if (_state.value.conn == Conn.Connected) notify(Uuids.ACCEL, on)
    }

    private fun open(auto: Boolean) {
        // Reconnects use autoConnect so Android keeps trying while the phone is in a pocket.
        gatt = device?.connectGatt(ctx, auto, callback, BluetoothDevice.TRANSPORT_LE, BluetoothDevice.PHY_LE_1M_MASK, handler)
    }

    private fun clearQueue() {
        queue.clear()
        cur = null
    }

    private fun enqueue(run: () -> Boolean) {
        queue.addLast(Op(run))
        pump()
    }

    private fun pump() {
        while (cur == null) {
            val op = queue.removeFirstOrNull() ?: return
            cur = op
            if (!op.run()) cur = null
        }
    }

    private fun next() {
        cur = null
        pump()
    }

    private fun finish(status: Int) {
        val op = cur ?: return
        if ((status == 5 || status == 15) && op.tries < 6) {
            op.tries++
            handler.postDelayed({ if (cur === op && !op.run()) next() }, 1500)
        } else {
            next()
        }
    }

    private fun read(u: UUID) {
        val c = chars[u] ?: return
        enqueue { gatt?.readCharacteristic(c) == true }
    }

    private fun write(u: UUID, v: Int) {
        val c = chars[u] ?: return
        enqueue {
            c.value = byteArrayOf(v.toByte())
            c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            gatt?.writeCharacteristic(c) == true
        }
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
        val reconnected = everConnected
        everConnected = true
        _state.update { it.copy(conn = Conn.Connected, error = null) }
        listOf(Uuids.FAN, Uuids.STROBE, Uuids.CHARGER, Uuids.BATTERY).forEach { notify(it, true) }
        if (live) notify(Uuids.ACCEL, true)
        listOf(Uuids.BATTERY, Uuids.FAN, Uuids.STROBE, Uuids.CHARGER, Uuids.MODEL, Uuids.MANUFACTURER).forEach(::read)
        if (reconnected) onReconnect(System.currentTimeMillis())
    }

    private fun onValue(u: UUID, v: ByteArray) {
        if (u == Uuids.ACCEL) {
            parseAccelFrame(v)?.let { onFrame(System.currentTimeMillis(), it) }
            return
        }
        if (v.isEmpty()) return
        val b = v[0].toInt() and 0xFF
        _state.update {
            when (u) {
                Uuids.FAN -> it.copy(fan = b)
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
                if (!g.requestMtu(247)) g.discoverServices()
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                clearQueue()
                g.close()
                gatt = null
                if (wantConnected && everConnected) {
                    _state.update { it.copy(conn = Conn.Reconnecting) }
                    open(true)
                } else {
                    wantConnected = false
                    _state.value = DeviceState(error = "Connection failed (status $status)")
                }
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
