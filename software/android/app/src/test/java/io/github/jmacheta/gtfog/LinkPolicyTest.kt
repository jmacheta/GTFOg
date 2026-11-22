package io.github.jmacheta.gtfog

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class LinkPolicyTest {
    @Test
    fun earlyDropsOfBondedPhoneMeanStaleBond() {
        val p = LinkPolicy()
        assertEquals(Next.GiveUp, p.onDisconnect(8, up = true, bonded = false)) // not bonded: never counted
        p.reset()
        assertEquals(Next.GiveUp, p.onDisconnect(8, up = true, bonded = true))
        assertEquals(Next.GiveUp, p.onDisconnect(8, up = true, bonded = true))
        assertEquals(Next.AuthFailed, p.onDisconnect(8, up = true, bonded = true))
    }

    @Test
    fun dropsBeforeTheLinkWasUpDoNotCount() {
        val p = LinkPolicy()
        repeat(5) { assertEquals(Next.GiveUp, p.onDisconnect(8, up = false, bonded = true)) }
    }

    @Test
    fun readyResetsEarlyDropsAndSwitchesToReconnect() {
        val p = LinkPolicy()
        p.onDisconnect(8, up = true, bonded = true)
        p.onDisconnect(8, up = true, bonded = true)
        p.ready()
        assertTrue(p.everConnected)
        assertEquals(Next.Reconnect, p.onDisconnect(8, up = true, bonded = true))
        assertEquals(Next.Reconnect, p.onDisconnect(8, up = true, bonded = true))
    }

    @Test
    fun authStatusesFailAtOnce() {
        for (s in listOf(5, 15, 137)) assertEquals(Next.AuthFailed, LinkPolicy().onDisconnect(s, up = false, bonded = false))
    }

    @Test
    fun firstConnectRetries133ThreeTimes() {
        val p = LinkPolicy()
        repeat(3) { assertEquals(Next.RetryFirst, p.onDisconnect(133, up = false, bonded = false)) }
        assertEquals(Next.GiveUp, p.onDisconnect(133, up = false, bonded = false))
        p.reset()
        assertEquals(Next.RetryFirst, p.onDisconnect(133, up = false, bonded = false))
    }

    @Test
    fun otherStatusOnFirstConnectGivesUpAndTimeoutToo() {
        assertEquals(Next.GiveUp, LinkPolicy().onDisconnect(8, up = false, bonded = false))
        assertEquals(Next.GiveUp, LinkPolicy().onLost())
    }

    @Test
    fun discoveryErrorRetriesAndMissingServiceIsNotGtfog() {
        val p = LinkPolicy()
        assertEquals(Next.RetryFirst, p.onDiscovered(129, true))
        assertEquals(Next.NotGtfog, p.onDiscovered(0, false))
        assertNull(p.onDiscovered(0, true))
        repeat(2) { p.onDiscovered(133, false) }
        assertEquals(Next.GiveUp, p.onDiscovered(133, false)) // 4th failure
        p.ready()
        assertEquals(Next.Reconnect, p.onDiscovered(133, false))
    }

    @Test
    fun bondLost() {
        assertEquals(true, bondLost(12, 10))
        assertEquals(false, bondLost(11, 10))
        assertNull(bondLost(10, 10))
        assertNull(bondLost(11, 12))
    }

    @Test
    fun pendingWritesReadBackOnlyAfterTheLast() {
        val w = PendingWrites<String>()
        assertFalse(w.has("fan"))
        w.begin("fan"); w.begin("fan"); w.begin("strobe")
        assertTrue(w.has("fan"))
        assertFalse(w.end("fan"))
        assertTrue(w.has("fan")) // notifications still ignored
        assertTrue(w.end("fan"))
        assertFalse(w.has("fan"))
        assertTrue(w.has("strobe"))
        w.clear()
        assertFalse(w.has("strobe"))
    }
}
