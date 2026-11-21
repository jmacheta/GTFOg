package io.github.jmacheta.gtfog

import java.util.Locale
import java.util.UUID
import kotlin.math.min
import kotlin.math.sqrt

object Uuids {
    val SERVICE: UUID = UUID.fromString("a7f00001-5c1e-4b8e-9d2a-6f0e1c3b4d50")
    val FAN: UUID = UUID.fromString("a7f00002-5c1e-4b8e-9d2a-6f0e1c3b4d50")
    val STROBE: UUID = UUID.fromString("a7f00003-5c1e-4b8e-9d2a-6f0e1c3b4d50")
    val ACCEL: UUID = UUID.fromString("a7f00004-5c1e-4b8e-9d2a-6f0e1c3b4d50")
    val CHARGER: UUID = UUID.fromString("a7f00005-5c1e-4b8e-9d2a-6f0e1c3b4d50")
    val BATTERY: UUID = UUID.fromString("00002a19-0000-1000-8000-00805f9b34fb")
    val MODEL: UUID = UUID.fromString("00002a24-0000-1000-8000-00805f9b34fb")
    val MANUFACTURER: UUID = UUID.fromString("00002a29-0000-1000-8000-00805f9b34fb")
    val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
}

const val CSV_HEADER = "phone_time_ms,seq,index,x_mg,y_mg,z_mg"
const val MAX_FRAME_SAMPLES = 40

data class Sample(val x: Int, val y: Int, val z: Int)

data class AccelFrame(val seq: Int, val rateHz: Int, val samples: List<Sample>)

private fun u8(b: ByteArray, i: Int) = b[i].toInt() and 0xFF
private fun i16(b: ByteArray, i: Int) = ((u8(b, i + 1) shl 8) or u8(b, i)).toShort().toInt()

/** Decodes seq, rate, n and n milli-g triples (little endian); null if malformed. */
fun parseAccelFrame(b: ByteArray): AccelFrame? {
    if (b.size < 4) return null
    val n = u8(b, 3)
    if (n > MAX_FRAME_SAMPLES || b.size < 4 + 6 * n) return null
    val samples = List(n) { Sample(i16(b, 4 + 6 * it), i16(b, 6 + 6 * it), i16(b, 8 + 6 * it)) }
    return AccelFrame(u8(b, 0) or (u8(b, 1) shl 8), u8(b, 2), samples)
}

fun magnitudeG(s: Sample): Double =
    sqrt(1.0 * s.x * s.x + 1.0 * s.y * s.y + 1.0 * s.z * s.z) / 1000.0

fun chargerText(raw: Int): String = when (raw) {
    0 -> "Not charging"
    1 -> "Charging"
    2 -> "Charge completed"
    3 -> "Fault"
    else -> "Unknown"
}

/** Counts frames and frames missing between consecutive sequence numbers (u16, wrapping). */
class SeqTracker {
    private var last = -1
    var frames = 0L
        private set
    var lost = 0L
        private set

    fun onFrame(seq: Int) {
        frames++
        if (last >= 0) {
            val gap = (seq - last - 1) and 0xFFFF
            if (gap >= 0x8000) return // duplicate or reordered frame
            lost += gap
        }
        last = seq
    }

    /** Forgets the last seq so a stream restart is not counted as loss. */
    fun resync() {
        last = -1
    }

    fun reset() {
        resync()
        frames = 0
        lost = 0
    }
}

/** Fixed-size ring of the newest samples; safe to use from several threads. */
class SampleRing(private val cap: Int) {
    private val xs = IntArray(cap)
    private val ys = IntArray(cap)
    private val zs = IntArray(cap)
    private var total = 0L

    @Synchronized
    fun add(s: Sample) {
        val i = (total % cap).toInt()
        xs[i] = s.x; ys[i] = s.y; zs[i] = s.z
        total++
    }

    @Synchronized
    fun clear() {
        total = 0
    }

    @Synchronized
    fun last(): Sample? {
        if (total == 0L) return null
        val i = ((total - 1) % cap).toInt()
        return Sample(xs[i], ys[i], zs[i])
    }

    /** Copies the newest min(n, stored) samples, oldest first, into out[0..2]; returns the count. */
    @Synchronized
    fun snapshot(n: Int, out: Array<IntArray>): Int {
        val m = min(min(n.toLong(), total), cap.toLong()).toInt()
        val start = total - m
        for (k in 0 until m) {
            val i = ((start + k) % cap).toInt()
            out[0][k] = xs[i]; out[1][k] = ys[i]; out[2][k] = zs[i]
        }
        return m
    }
}

/**
 * Samples per second. Samples come in frames, so windows start and end when a frame arrives
 * (total changed) and span at least one second; otherwise 40-sample frames make the rate jump.
 */
class RateMeter {
    private var t0 = -1L
    private var c0 = 0L
    private var last = 0L
    private var rate = 0.0

    fun update(nowMs: Long, total: Long): Double {
        val arrived = total != last
        last = total
        if (t0 < 0 || total < c0) {
            t0 = nowMs; c0 = total
        } else if (arrived && nowMs - t0 >= 1000) {
            rate = (total - c0) * 1000.0 / (nowMs - t0)
            t0 = nowMs; c0 = total
        }
        return rate
    }
}

private val AUTO_RANGES = doubleArrayOf(1.0, 1.5, 2.0, 3.0, 4.0, 6.0, 8.0, 12.0, 16.0)

/** Smallest symmetric Y range in g that holds maxAbsG with 10 % headroom. */
fun autoRangeG(maxAbsG: Double): Double =
    AUTO_RANGES.firstOrNull { it >= maxAbsG * 1.1 } ?: AUTO_RANGES.last()

fun csvLines(phoneMs: Long, f: AccelFrame): String = buildString {
    f.samples.forEachIndexed { i, s -> append("$phoneMs,${f.seq},$i,${s.x},${s.y},${s.z}\n") }
}

fun csvPreamble(startIso: String, info: String): String = "# start=$startIso, $info\n$CSV_HEADER\n"

fun csvReconnect(ms: Long): String = "# reconnect at $ms\n"

fun formatDuration(ms: Long): String {
    val s = ms / 1000
    return String.format(Locale.US, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60)
}
