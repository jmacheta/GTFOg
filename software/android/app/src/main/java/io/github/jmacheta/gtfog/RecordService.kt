package io.github.jmacheta.gtfog

import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.IBinder
import android.os.PowerManager
import androidx.core.app.ServiceCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch

/** Foreground service that keeps the process, and so the GATT link and the CSV writer, alive while recording. */
@SuppressLint("MissingPermission")
class RecordService : Service() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var job: Job? = null
    private var wakeLock: PowerManager.WakeLock? = null
    private val session get() = (application as GtfogApp).session

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            session.stopRecording()
            stopSelf()
            return START_NOT_STICKY
        }
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel(CHANNEL, "Recording", NotificationManager.IMPORTANCE_LOW))
        try {
            ServiceCompat.startForeground(this, ID, notification(), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        } catch (e: RuntimeException) { // e.g. ForegroundServiceStartNotAllowedException (API 31+) when started in the background
            session.recError.value = "Recording failed: the system did not allow the background service (${e.message ?: e.javaClass.simpleName})"
            session.stopRecording()
            stopSelf()
            return START_NOT_STICKY
        }
        if (wakeLock == null) {
            wakeLock = getSystemService(PowerManager::class.java)
                .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "gtfog:record").also { it.acquire(WAKE_LOCK_MS) }
        }
        job?.cancel()
        job = scope.launch {
            val refresh = launch {
                while (true) {
                    nm.notify(ID, notification())
                    delay(5000) // the duration is a chronometer; only the counters need a refresh
                }
            }
            session.rec.first { !it.active } // Stop, or the link ended: the notification goes at once
            refresh.cancel()
            stopForeground(STOP_FOREGROUND_REMOVE)
            stopSelf()
        }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        scope.cancel()
        wakeLock?.release()
        super.onDestroy()
    }

    private fun notification(): Notification {
        val r = session.rec.value
        val lost = session.stats.value.lost
        val state = session.client.state.value.conn.text()
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        val stop = PendingIntent.getService(
            this, 1, Intent(this, RecordService::class.java).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE,
        )
        return Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_launcher_foreground)
            .setContentTitle("Recording ($state)")
            .setWhen(r.startMs)
            .setUsesChronometer(true)
            .setContentText("${r.samples} samples, ${lost} lost frames")
            .setContentIntent(open)
            .setOngoing(true)
            .addAction(Notification.Action.Builder(null, "Stop", stop).build())
            .build()
    }

    companion object {
        private const val WAKE_LOCK_MS = 4 * 3600_000L // safety net; a longer recording keeps running, the CPU may sleep
        private const val ID = 1
        private const val CHANNEL = "recording"
        private const val ACTION_STOP = "io.github.jmacheta.gtfog.STOP"
    }
}
