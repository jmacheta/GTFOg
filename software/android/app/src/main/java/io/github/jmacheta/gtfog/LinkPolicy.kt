package io.github.jmacheta.gtfog

private const val FIRST_CONNECT_RETRIES = 3 // status 133 on the first direct connect is a common Android flake
private const val EARLY_DROPS = 3 // bonded phone, link dropped before the services were found
private const val EARLY_WINDOW_MS = 120_000L // ... and all of them within this time (the goggles switching off now and then is not a bad key)

/** Only the statuses of an ordinary disconnect count as early drops; 8, 62, 133 are range and radio problems, not a bad key. */
private val EARLY_DROP_STATUSES = setOf(0, 19, 22)

/** What [GtfogClient] does after a link problem. */
enum class Next { AuthFailed, Reconnect, RetryFirst, GiveUp, NotGtfog }

/** Android BluetoothDevice bond states (BOND_NONE, BOND_BONDED), kept here so the policy has no Android types. */
private const val BOND_NONE = 10
private const val BOND_BONDED = 12

/**
 * The reconnect decisions of [GtfogClient] without Android types: early drops mean a stale bond, a failed first connect
 * is retried a few times, a failed service discovery is a retry, not "Not a GTFOg".
 */
class LinkPolicy {
    /** The services were ready at least once since [reset]. */
    var everConnected = false
        private set
    private var firstFails = 0
    private var earlyDrops = 0
    private var firstEarlyMs = 0L

    /** A new connection requested by the user. */
    fun reset() {
        everConnected = false
        firstFails = 0
        earlyDrops = 0
    }

    /** The services are found and the link is usable. */
    fun ready() {
        everConnected = true
        earlyDrops = 0
    }

    /** The GATT link ended with [status]; [up] = it had connected but the services were not ready, [bonded] = the phone has a bond. */
    fun onDisconnect(status: Int, up: Boolean, bonded: Boolean, nowMs: Long = System.currentTimeMillis()): Next {
        var early = false
        if (up && bonded && status in EARLY_DROP_STATUSES) {
            if (earlyDrops == 0 || nowMs - firstEarlyMs > EARLY_WINDOW_MS) {
                earlyDrops = 0
                firstEarlyMs = nowMs
            }
            early = ++earlyDrops >= EARLY_DROPS
        }
        return if (isAuthFailure(status) || early) Next.AuthFailed else onLost(status == 133)
    }

    /** The link is gone or never came up (also a connect timeout: [retryFirst] false). */
    fun onLost(retryFirst: Boolean = false): Next = when {
        everConnected -> Next.Reconnect
        retryFirst && firstFails++ < FIRST_CONNECT_RETRIES -> Next.RetryFirst
        else -> Next.GiveUp
    }

    /** Service discovery finished: an error is a lost link, a missing service is the wrong device. */
    fun onDiscovered(status: Int, hasGtfogServices: Boolean): Next? = when {
        status != 0 -> onLost(true)
        !hasGtfogServices -> Next.NotGtfog
        else -> null
    }
}

/** A bond state change: true = the phone had a bond that is gone, false = a pairing was refused, null = not a failure. */
fun bondLost(previous: Int, now: Int): Boolean? = if (now == BOND_NONE && previous != BOND_NONE) previous == BOND_BONDED else null

/**
 * Writes queued or in flight per key. While any exist, notifications and reads of that key are ignored: they carry
 * older values and would overwrite the newer optimistic one.
 */
class PendingWrites<K> {
    private val counts = HashMap<K, Int>()

    fun has(key: K) = key in counts

    fun begin(key: K) {
        counts[key] = (counts[key] ?: 0) + 1
    }

    /** One write ended; true when it was the last of the burst, so the value should be read back now. */
    fun end(key: K): Boolean {
        val n = (counts[key] ?: 0) - 1
        if (n > 0) counts[key] = n else counts.remove(key)
        return n <= 0
    }

    fun clear() = counts.clear()
}
