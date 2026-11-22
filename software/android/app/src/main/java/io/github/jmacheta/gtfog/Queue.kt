package io.github.jmacheta.gtfog

import kotlin.math.min

/** Passed to [Op.onEnd] instead of a GATT status: the operation timed out, or Android refused to start it. */
const val OP_TIMEOUT = -1
const val OP_NOT_STARTED = -2

/** Statuses that mean "not encrypted yet": retried while Android pairs. */
fun isRetryStatus(status: Int) = status == 5 || status == 15

/** Statuses that, after the retries, mean the bond is bad (insufficient authentication/encryption, GATT_AUTH_FAIL). */
fun isAuthFailure(status: Int) = status == 5 || status == 15 || status == 137

/** Delayed callbacks; the Android client wraps a Handler, tests use a fake clock. */
interface Timer {
    fun postDelayed(r: Runnable, ms: Long)
    fun remove(r: Runnable)
}

/**
 * [run] starts the GATT call (false: refused); [onEnd] gets the final status exactly once (not for retried ones).
 * [key] names what the call is about (characteristic or descriptor): a callback with another key is a late one of an abandoned op.
 */
class Op(val run: () -> Boolean, val onEnd: (Int) -> Unit = {}, val key: Any? = null) {
    var tries = 0
}

/**
 * Serial GATT operation queue without Android types. One operation at a time; 5/15 are retried every [retryMs]
 * up to [maxTries] times (without limit while [isBonding]); an operation without a callback ends with [OP_TIMEOUT]
 * after [timeoutMs] (not while bonding). An operation that still ends in 5/15 calls [onAuthFailed].
 */
class OpQueue(
    private val timer: Timer,
    private val isBonding: () -> Boolean,
    private val onAuthFailed: () -> Unit = {},
    private val timeoutMs: Long = 5_000,
    private val retryMs: Long = 1_500,
    private val maxTries: Int = 6,
) {
    private val queue = ArrayDeque<Op>()
    private var cur: Op? = null

    val busy get() = cur != null

    private val timeout = object : Runnable {
        override fun run() {
            val op = cur ?: return
            if (isBonding()) timer.postDelayed(this, timeoutMs) else end(op, OP_TIMEOUT)
        }
    }

    fun enqueue(op: Op) {
        queue.addLast(op)
        pump()
    }

    /** Drops everything without calling [Op.onEnd] (the link is gone). */
    fun clear() {
        timer.remove(timeout)
        queue.clear()
        cur = null
    }

    /** A GATT callback arrived for the current operation; one whose [key] differs from the current op's is ignored. */
    fun finish(status: Int, key: Any? = null) {
        val op = cur ?: return
        if (key != null && op.key != null && op.key != key) return
        if (isRetryStatus(status) && (op.tries < maxTries || isBonding())) {
            op.tries++
            timer.postDelayed({ if (cur === op) restart(op) }, retryMs)
        } else {
            end(op, status)
        }
    }

    private fun start(op: Op): Boolean {
        cur = op
        timer.remove(timeout)
        timer.postDelayed(timeout, timeoutMs)
        return op.run()
    }

    private fun restart(op: Op) {
        if (!start(op)) end(op, OP_NOT_STARTED)
    }

    private fun pump() {
        while (cur == null) {
            val op = queue.removeFirstOrNull() ?: return
            if (!start(op)) end(op, OP_NOT_STARTED, pump = false)
        }
    }

    private fun end(op: Op, status: Int, pump: Boolean = true) {
        timer.remove(timeout)
        cur = null
        op.onEnd(status)
        if (isRetryStatus(status)) onAuthFailed()
        if (pump) pump()
    }
}

/** Reconnect delays 1, 2, 4 ... 30 s; [reset] after a successful connection. */
class Backoff(private val minMs: Long = 1_000, private val maxMs: Long = 30_000) {
    private var n = 0

    fun next(): Long = min(maxMs, minMs shl n).also { if (it < maxMs) n++ }

    fun reset() {
        n = 0
    }
}
