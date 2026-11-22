package io.github.jmacheta.gtfog

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

private class FakeTimer : Timer {
    var now = 0L
    private val tasks = ArrayList<Pair<Long, Runnable>>()

    override fun postDelayed(r: Runnable, ms: Long) {
        tasks += now + ms to r
    }

    override fun remove(r: Runnable) {
        tasks.removeAll { it.second === r }
    }

    fun advance(ms: Long) {
        val end = now + ms
        while (true) {
            val t = tasks.filter { it.first <= end }.minByOrNull { it.first } ?: break
            tasks.remove(t)
            now = t.first
            t.second.run()
        }
        now = end
    }
}

class QueueTest {
    private val timer = FakeTimer()
    private var bonding = false
    private var authFailed = 0
    private var stuck = 0
    private val q = OpQueue(timer, { bonding }, { authFailed++ }, { stuck++ })
    private val log = ArrayList<String>()

    private fun op(name: String, start: Boolean = true) =
        Op({ log += "run $name"; start }, { log += "end $name $it" })

    @Test
    fun runsOneAtATimeInOrder() {
        q.enqueue(op("a")); q.enqueue(op("b"))
        assertEquals(listOf("run a"), log)
        q.finish(0)
        assertEquals(listOf("run a", "end a 0", "run b"), log)
        q.finish(0)
        assertFalse(q.busy)
    }

    @Test
    fun retriesAuthErrorsThenReports() {
        q.enqueue(op("a")); q.enqueue(op("b"))
        repeat(6) { q.finish(5); timer.advance(1500) }
        assertEquals(7, log.count { it == "run a" })
        assertEquals(0, authFailed)
        q.finish(15)
        assertEquals(listOf("end a 15", "run b"), log.takeLast(2))
        assertEquals(1, authFailed)
    }

    @Test
    fun retriesWithoutLimitWhileBonding() {
        bonding = true
        q.enqueue(op("a"))
        repeat(20) { q.finish(5); timer.advance(1500) }
        assertEquals(21, log.count { it == "run a" })
        bonding = false
        q.finish(0)
        assertTrue(log.contains("end a 0"))
    }

    @Test
    fun timeoutEndsTheOperationAndStartsTheNext() {
        q.enqueue(op("a")); q.enqueue(op("b"))
        timer.advance(4999)
        assertFalse(log.contains("end a -1"))
        timer.advance(1)
        assertEquals(listOf("run a", "end a $OP_TIMEOUT", "run b"), log)
    }

    @Test
    fun timeoutWaitsWhileBonding() {
        bonding = true
        q.enqueue(op("a"))
        timer.advance(20_000)
        assertEquals(listOf("run a"), log)
        bonding = false
        timer.advance(5_000)
        assertTrue(log.contains("end a $OP_TIMEOUT"))
    }

    @Test
    fun lateCallbackOfTimedOutOpIsIgnored() {
        q.enqueue(Op({ log += "run a"; true }, { log += "end a $it" }, "a")); q.enqueue(Op({ log += "run b"; true }, { log += "end b $it" }, "b"))
        timer.advance(5_000)
        assertEquals(listOf("run a", "end a $OP_TIMEOUT", "run b"), log)
        q.finish(0, "a") // the late answer of a
        assertTrue(q.busy)
        q.finish(0, "b")
        assertEquals("end b 0", log.last())
    }

    @Test
    fun lateCallbackOfTimedOutOpDoesNotCompleteNewerOpWithSameKey() {
        q.enqueue(Op({ log += "run a1"; true }, { log += "end a1 $it" }, "a")); q.enqueue(Op({ log += "run a2"; true }, { log += "end a2 $it" }, "a"))
        timer.advance(5_000)
        assertEquals(listOf("run a1", "end a1 $OP_TIMEOUT", "run a2"), log)
        q.finish(0, "a") // late answer of a1: swallowed
        assertTrue(q.busy)
        q.finish(0, "a") // the real answer of a2
        assertEquals("end a2 0", log.last())
        assertFalse(q.busy)
    }

    @Test
    fun lateAnswerThatNeverComesDoesNotSwallowALaterGenuineOne() {
        q.enqueue(Op({ log += "run a1"; true }, { log += "end a1 $it" }, "a"))
        timer.advance(5_000) // a1 timed out
        timer.advance(5_000) // the late answer did not come within one more timeout
        q.enqueue(Op({ log += "run a2"; true }, { log += "end a2 $it" }, "a"))
        q.finish(0, "a")
        assertEquals("end a2 0", log.last())
        assertFalse(q.busy)
    }

    @Test
    fun cccdKeyDoesNotMatchCharacteristicKey() {
        q.enqueue(Op({ true }, { log += "end $it" }, "cccd fan"))
        q.finish(0, "fan")
        assertTrue(q.busy)
        q.finish(0, "cccd fan")
        assertEquals(listOf("end 0"), log)
    }

    @Test
    fun threeTimeoutsOrRefusalsInARowAreStuck() {
        repeat(3) { q.enqueue(op("t$it")) }
        timer.advance(5_000); timer.advance(5_000)
        assertEquals(0, stuck)
        timer.advance(5_000)
        assertEquals(1, stuck)
        repeat(3) { q.enqueue(op("r$it", start = false)) } // refused: the same escalation, counted again from zero
        assertEquals(2, stuck)
    }

    @Test
    fun realAnswerResetsTheStuckCount() {
        repeat(2) { q.enqueue(op("r$it", start = false)) }
        q.enqueue(op("ok")); q.finish(0)
        repeat(2) { q.enqueue(op("s$it", start = false)) }
        assertEquals(0, stuck)
    }

    @Test
    fun refusedOperationEndsAndNextRuns() {
        q.enqueue(op("a", start = false)); q.enqueue(op("b"))
        assertEquals(listOf("run a", "end a $OP_NOT_STARTED", "run b"), log)
    }

    @Test
    fun operationEnqueuedFromOnEndRunsNext() {
        q.enqueue(Op({ log += "run w"; true }, { q.enqueue(op("readback")) }))
        q.enqueue(op("b"))
        q.finish(3)
        assertEquals(listOf("run w", "run b"), log)
        q.finish(0)
        assertEquals(listOf("run w", "run b", "end b 0", "run readback"), log)
    }

    @Test
    fun clearDropsEverythingAndStaleRetryDoesNothing() {
        q.enqueue(op("a")); q.enqueue(op("b"))
        q.finish(5)
        q.clear()
        timer.advance(10_000)
        assertEquals(listOf("run a"), log)
        assertFalse(q.busy)
    }

    @Test
    fun backoffDoublesToThirtySecondsAndResets() {
        val b = Backoff()
        assertEquals(listOf(1000L, 2000, 4000, 8000, 16000, 30000, 30000), List(7) { b.next() })
        b.reset()
        assertEquals(1000L, b.next())
    }
}
