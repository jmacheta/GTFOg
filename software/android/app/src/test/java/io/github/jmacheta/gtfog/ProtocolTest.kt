package io.github.jmacheta.gtfog

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Assert.assertFalse
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
    fun seqGapBoundaryIsLostBelow0x8000() {
        val t = SeqTracker()
        listOf(0, 0x8000).forEach(t::onFrame) // gap 0x7FFF: counted as lost
        assertEquals(0x7FFF, t.lost)
        val u = SeqTracker()
        listOf(0, 0x8001).forEach(u::onFrame) // gap 0x8000: treated as reordered
        assertEquals(0, u.lost)
    }

    @Test
    fun frameIgnoresTrailingBytes() {
        val f = parseAccelFrame(le16(1) + byteArrayOf(100, 1) + ByteArray(6) + byteArrayOf(9, 9))!!
        assertEquals(1, f.samples.size)
    }

    @Test
    fun stepFanClamps() {
        assertEquals(55, stepFan(50, 1, 5))
        assertEquals(0, stepFan(3, -1, 5))
        assertEquals(100, stepFan(98, 1, 20))
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
        assertArrayEquals(byteArrayOf(3, 30, 5, 0, 60, 20, 50, 100), DeviceSettings(true, true, 30).encode())
        assertArrayEquals(byteArrayOf(2, 0, 10, 1, 120, 0, 10, 90), DeviceSettings(false, true, 0, 10, true, 120, listOf(0, 10, 90)).encode())
        assertEquals(
            DeviceSettings(true, false, 5, 2, true, 15, listOf(50, 60, 70)),
            parseSettings(byteArrayOf(1, 5, 2, 1, 15, 50, 60, 70)),
        )
        assertEquals(
            DeviceSettings(false, true, 60, 25, false, 255, listOf(100, 0, 100)),
            parseSettings(byteArrayOf(2, 60, 25, 0, -1, 100, 0, 100)),
        )
    }

    @Test
    fun oldFirmwareSettings() {
        val s = parseSettings(byteArrayOf(1, 5))!!
        assertEquals(DeviceSettings(true, false, 5, size = 2), s)
        assertEquals(5, s.fanStep)
        assertEquals(60, s.motionDelayS)
        // written back with the length the device uses
        assertArrayEquals(byteArrayOf(2, 60), DeviceSettings(false, true, 60, size = 2).encode())
        assertArrayEquals(byteArrayOf(1, 5), s.encode())
    }

    @Test
    fun firmware031Settings() {
        val s = parseSettings(byteArrayOf(1, 5, 10, 1, 30, 40))!!
        assertEquals(DeviceSettings(true, false, 5, 10, true, 30, listOf(40, 50, 100), size = 6), s)
        assertEquals(false, s.perActivity)
        assertArrayEquals(byteArrayOf(1, 5, 10, 1, 30, 40), s.encode())
        assertArrayEquals(byteArrayOf(1, 5, 10, 1, 30, 70), s.copy(motionSpeeds = listOf(70, 50, 100)).encode())
    }

    @Test
    fun settingsEncodeClamps() {
        assertArrayEquals(byteArrayOf(0, 0, 1, 0, 5, 0, 0, 0), DeviceSettings(false, false, 0, 0, false, 0, listOf(-3, -1, 0)).encode())
        assertArrayEquals(byteArrayOf(0, -1, 25, 1, -1, 100, 100, 100), DeviceSettings(false, false, 999, 99, true, 999, listOf(200, 101, 100)).encode())
    }

    @Test
    fun settingsRejectWrongSize() {
        for (n in listOf(0, 1, 3, 4, 5, 7)) assertNull(parseSettings(ByteArray(n)))
    }

    @Test
    fun settingsIgnoreTrailingBytes() {
        val s = parseSettings(byteArrayOf(2, 60, 10, 1, 120, 0, 10, 90, 7, 7))!!
        assertEquals(DeviceSettings(false, true, 60, 10, true, 120, listOf(0, 10, 90)), s)
        assertEquals(8, s.encode().size)
    }

    @Test
    fun fanActual() {
        assertEquals(FanActual(80, 2), parseFanActual(byteArrayOf(80, 2)))
        assertEquals(FanActual(0, 0), parseFanActual(byteArrayOf(0, 0)))
        assertEquals(FanActual(50, null), parseFanActual(byteArrayOf(50, -1))) // 255: manual mode
        assertEquals(FanActual(50, null), parseFanActual(byteArrayOf(50))) // firmware 0.3.0 and 0.3.1
        assertNull(parseFanActual(ByteArray(0)))
        assertEquals(FanActual(80, 2), parseFanActual(byteArrayOf(80, 2, 9, 9))) // future firmware
    }

    @Test
    fun labelsAndTexts() {
        assertEquals("GTFOg \u00b7 B3:0B", deviceLabel("GTFOg", "AA:BB:CC:DD:B3:0B"))
        assertEquals("Battery 82 %, charging", batteryDescription(82, 1))
        assertEquals("Battery 82 %", batteryDescription(82, 0))
        assertEquals("Battery level unknown", batteryDescription(null, null))
        assertEquals("Battery, charger fault", batteryDescription(50, 3))
        assertEquals("Out of range", gattErrorText(8))
        assertEquals("The goggles closed the connection", gattErrorText(19))
        assertEquals("Connection closed", gattErrorText(22))
        assertEquals("Could not connect, try again", gattErrorText(133))
        assertEquals("Connection failed (status 7)", gattErrorText(7))
    }

    @Test
    fun settingsOutOfRangeAreClamped() {
        val s = parseSettings(byteArrayOf(0, 5, 0, 1, 0, 101, 120, -1))!!
        assertEquals(1, s.fanStep)
        assertEquals(5, s.motionDelayS)
        assertEquals(listOf(100, 100, 100), s.motionSpeeds)
        assertEquals(25, parseSettings(byteArrayOf(0, 5, 99, 0, 60, 1, 1, 1))!!.fanStep)
    }

    @Test
    fun dfuBatteryGate() {
        assertNotNull(dfuBlockReason(49))
        assertNull(dfuBlockReason(50))
        assertNull(dfuBlockReason(100))
        assertNotNull(dfuBlockReason(null))
        assertEquals("Charge to at least 50 % to update (now 49 %)", dfuBlockReason(49))
    }

    @Test
    fun authFailureStatuses() {
        assertTrue(listOf(5, 15, 137).all(::isAuthFailure))
        assertFalse(isAuthFailure(133))
    }

    @Test
    fun unknownFlagBitsAreWrittenBack() {
        val s = parseSettings(byteArrayOf(0x83.toByte(), 10, 5, 0, 60, 20, 50, 100))!!
        assertTrue(s.stealth && s.transportLock)
        assertArrayEquals(byteArrayOf(0x82.toByte(), 10, 5, 0, 60, 20, 50, 100), s.copy(stealth = false).encode())
        assertArrayEquals(byteArrayOf(0x84.toByte(), 0), parseSettings(byteArrayOf(0x84.toByte(), 0))!!.encode())
    }

    @Test
    fun settingsRoundTripForEverySize() {
        for (b in listOf(byteArrayOf(1, 5), byteArrayOf(2, 0, 10, 1, 120, 40), byteArrayOf(3, 30, 5, 0, 60, 20, 50, 100))) {
            assertArrayEquals(b, parseSettings(b)!!.encode())
        }
    }
}
