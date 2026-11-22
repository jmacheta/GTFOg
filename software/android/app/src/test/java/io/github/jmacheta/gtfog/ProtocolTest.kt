package io.github.jmacheta.gtfog

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class ProtocolTest {
    private fun le16(v: Int) = byteArrayOf(v.toByte(), (v shr 8).toByte())

    @Test
    fun parsesFrame() {
        val b = le16(0x1234) + byteArrayOf(100, 2) + le16(1000) + le16(-1) + le16(-16000) + le16(0) + le16(32767) + le16(-32768)
        val f = parseAccelFrame(b)!!
        assertEquals(0x1234, f.seq)
        assertEquals(100, f.rateHz)
        assertEquals(listOf(Sample(1000, -1, -16000), Sample(0, 32767, -32768)), f.samples)
    }

    @Test
    fun rejectsMalformed() {
        assertNull(parseAccelFrame(byteArrayOf(0, 0, 100)))
        assertNull(parseAccelFrame(le16(1) + byteArrayOf(100, 2) + ByteArray(11)))
        assertNull(parseAccelFrame(le16(1) + byteArrayOf(100, 41) + ByteArray(246)))
    }

    @Test
    fun emptyFrameIsValid() {
        assertEquals(0, parseAccelFrame(le16(5) + byteArrayOf(100, 0))!!.samples.size)
    }

    @Test
    fun seqGapsAndWrap() {
        val t = SeqTracker()
        listOf(10, 11, 14, 15).forEach(t::onFrame)
        assertEquals(2, t.lost)
        assertEquals(4, t.frames)
        val w = SeqTracker()
        listOf(65534, 65535, 0, 1).forEach(w::onFrame)
        assertEquals(0, w.lost)
        w.onFrame(4)
        assertEquals(2, w.lost)
    }

    @Test
    fun seqDuplicateAndResync() {
        val t = SeqTracker()
        listOf(5, 5, 4, 6).forEach(t::onFrame)
        assertEquals(0, t.lost)
        t.resync()
        t.onFrame(100)
        assertEquals(0, t.lost)
    }

    @Test
    fun csvFormat() {
        val f = AccelFrame(7, 100, listOf(Sample(1, -2, 3), Sample(4, 5, -6)))
        assertEquals("1000,7,0,1,-2,3\n1000,7,1,4,5,-6\n", csvLines(1000, f))
        assertEquals("# start=T, app=x\nphone_time_ms,seq,index,x_mg,y_mg,z_mg\n", csvPreamble("T", "app=x"))
        assertEquals("# reconnect at 42\n", csvReconnect(42))
    }

    @Test
    fun chargerMapping() {
        assertEquals("Charging", chargerText(1))
        assertEquals("Not charging", chargerText(0))
        assertEquals("Charge complete", chargerText(2))
        assertEquals("Fault", chargerText(3))
        assertEquals("Unknown", chargerText(9))
    }

    @Test
    fun ringKeepsNewest() {
        val r = SampleRing(4)
        for (i in 1..6) r.add(Sample(i, 0, -i))
        val out = Array(3) { IntArray(4) }
        assertEquals(3, r.snapshot(3, out))
        assertEquals(listOf(4, 5, 6), out[0].take(3))
        assertEquals(listOf(-4, -5, -6), out[2].take(3))
        assertEquals(4, r.snapshot(10, out))
        assertEquals(Sample(6, 0, -6), r.last())
    }

    @Test
    fun rateMeter() {
        val m = RateMeter()
        m.update(0, 0)
        assertEquals(0.0, m.update(500, 50), 0.0)
        assertEquals(100.0, m.update(1000, 100), 0.001)

        // 40-sample frames every 400 ms, polled every 16 ms like the UI: steady 100/s, not 80 or 120.
        val f = RateMeter()
        var r = 0.0
        for (t in 0L..10_000L step 16) r = f.update(t, t / 400 * 40)
        assertEquals(100.0, r, 5.0)
    }

    @Test
    fun rangeAndFormat() {
        assertEquals(1.0, autoRangeG(0.5), 0.0)
        assertEquals(3.0, autoRangeG(2.0), 0.0)
        assertEquals(16.0, autoRangeG(30.0), 0.0)
        assertEquals(1.0, magnitudeG(Sample(0, 1000, 0)), 1e-9)
        assertEquals("1:02:03", formatDuration(3723000))
    }

    @Test
    fun settingsRoundTrip() {
        assertArrayEquals(byteArrayOf(3, 30), DeviceSettings(true, true, 30).encode())
        assertArrayEquals(byteArrayOf(2, 0), DeviceSettings(false, true, 0).encode())
        assertEquals(DeviceSettings(true, false, 5), parseSettings(byteArrayOf(1, 5)))
        assertEquals(DeviceSettings(false, true, 60), parseSettings(byteArrayOf(2, 60)))
    }

    @Test
    fun settingsRejectWrongSize() {
        assertNull(parseSettings(byteArrayOf()))
        assertNull(parseSettings(byteArrayOf(1)))
        assertNull(parseSettings(byteArrayOf(1, 2, 3)))
    }
}
