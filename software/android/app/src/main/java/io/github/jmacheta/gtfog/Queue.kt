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
    /** Unique per operation: a late callback is matched by key, so the id tells an old timed-out op from the current one. */
    val id = ++count
    var tries = 0

    private companion object {
        var count = 0L
    }
}

/**
 * Serial GATT operation queue without Android types. One operation at a time; 5/15 are retried every [retryMs]
 * up to [maxTries] times (without limit while [isBonding]); an operation without a callback ends with [OP_TIMEOUT]
 * after [timeoutMs] (not while bonding). An operation that still ends in 5/15 calls [onAuthFailed]. After [maxStuck]
 * timeouts or refusals in a row the GATT stack is wedged: [onStuck] is called (the owner closes the link and reconnects).
 */
class OpQueue(
    private val timer: Timer,
    private val isBonding: () -> Boolean,
    private val onAuthFailed: () -> Unit = {},
    private val onStuck: () -> Unit = {},
    private val timeoutMs: Long = 5_000,
    private val retryMs: Long = 1_500,
    private val maxTries: Int = 6,
    private val maxStuck: Int = 3,
) {
    private val queue = ArrayDeque<Op>()
    private var cur: Op? = null

    /** The last op that ended by timeout: its answer may still arrive and must not complete a newer op with the same key. */
    private var late: Op? = null
    private var stuck = 0

    val busy get() = cur != null

    private val timeout = object : Runnable {
        override fun run() {
            val op = cur ?: return
            if (isBonding()) {
                timer.postDelayed(this, timeoutMs)
            } else {
                late = op
                end(op, OP_TIMEOUT)
            }
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
        late = null
        stuck = 0
    }

    /** A GATT callback arrived for the current operation; one whose [key] differs from the current op's is ignored. */
    fun finish(status: Int, key: Any? = null) {
        val op = cur ?: return
        if (key != null && op.key != null && op.key != key) return
        // The answer of the timed-out op (same key, older id) arrives once; it is not the current op's.
        late?.let { l -> if (l.id != op.id && key != null && l.key == key) { late = null; return } }
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
        if (status >= 0) late = null
        op.onEnd(status)
        if (isRetryStatus(status)) onAuthFailed()
        if (status == OP_TIMEOUT || status == OP_NOT_STARTED) {
            if (++stuck >= maxStuck) {
                stuck = 0
                onStuck()
            }
        } else stuck = 0
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
