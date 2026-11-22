@file:SuppressLint("MissingPermission")

package io.github.jmacheta.gtfog

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.selection.toggleable
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.core.content.FileProvider
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.repeatOnLifecycle
import kotlinx.coroutines.delay
import java.io.File
import java.util.Locale

@Composable
fun DebugTab(s: Session, st: DeviceState) {
    AccelCard(s, st)
    Recordings(s)
}

@Composable
private fun AccelCard(s: Session, st: DeviceState) {
    val ctx = LocalContext.current
    val stats by s.stats.collectAsStateWithLifecycle()
    val rec by s.rec.collectAsStateWithLifecycle()
    val recError by s.recError.collectAsStateWithLifecycle()
    var windowS by rememberSaveable { mutableStateOf(10) }
    var range by rememberSaveable { mutableStateOf<Double?>(null) }
    val tick = remember { mutableLongStateOf(0) }
    var nowSec by remember { mutableLongStateOf(0) }
    var mag by remember { mutableStateOf(0.0) }
    var rate by remember { mutableStateOf(0.0) }
    val meter = remember { RateMeter() }
    val askNotif = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { s.startRecording() }

    // Redraw at 30 Hz only while data flows (Live or recording) and the app is visible; otherwise once.
    val active = st.live || rec.active
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    LaunchedEffect(active) {
        var textAt = 0L
        lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
            while (true) {
                val now = System.currentTimeMillis()
                tick.longValue = now
                // The texts change at 4 Hz at most; only the chart redraws every frame.
                if (!active || now - textAt >= 250) {
                    textAt = now
                    nowSec = now / 1000
                    mag = Math.round((s.ring.last()?.let(::magnitudeG) ?: 0.0) * 100) / 100.0
                    rate = meter.update(now, s.stats.value.samples)
                }
                if (!active) break
                delay(33)
            }
        }
    }

    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            val canLive = st.conn == Conn.Connected
            Row(
                Modifier.toggleable(st.live, canLive, Role.Switch) { s.setLive(it) }.heightIn(min = TouchHeight),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text("Accelerometer", Modifier.weight(1f), style = MaterialTheme.typography.titleMedium)
                Text("Live  ")
                Switch(st.live, null, enabled = canLive)
            }
            st.mtu?.takeIf { it < 247 }?.let {
                Text("ATT MTU is $it (247 needed): accelerometer frames may be cut.", color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
            }
            ChoiceChips(WINDOWS.map { "$it s" to it }, windowS) { windowS = it }
            ChoiceChips(RANGES.map { (it?.let { r -> "±${r.toInt()} g" } ?: "auto") to it }, range) { range = it }
            AccelChart(s.ring, windowS, range, tick, "Accelerometer chart, last $windowS s, |a| ${String.format(Locale.US, "%.2f", mag)} g")
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
                AXIS_NAMES.forEachIndexed { i, n ->
                    Swatch(AXIS_COLORS[i]); Text(n)
                }
            }
            Text(
                String.format(Locale.US, "|a| %.2f g   %.0f samples/s   lost frames %d", mag, rate, stats.lost),
                style = MaterialTheme.typography.bodyMedium,
            )
            recError?.let { Text(it, color = MaterialTheme.colorScheme.error) }
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                if (rec.active) {
                    Button({ s.stopRecording() }, Modifier.heightIn(min = TouchHeight)) { Text("Stop") }
                } else {
                    Button(
                        {
                            if (Build.VERSION.SDK_INT >= 33 &&
                                ContextCompat.checkSelfPermission(ctx, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
                            ) askNotif.launch(Manifest.permission.POST_NOTIFICATIONS) else s.startRecording()
                        },
                        Modifier.heightIn(min = TouchHeight),
                        enabled = st.conn == Conn.Connected,
                    ) { Text("Record") }
                }
                if (rec.file.isNotEmpty()) {
                    val ms = (if (rec.active) nowSec * 1000 else rec.endMs) - rec.startMs
                    Text(
                        "${rec.file}\n${formatDuration(ms)}  ${rec.samples} samples",
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
            }
        }
    }
}

@Composable
private fun Recordings(s: Session) {
    var confirm by remember { mutableStateOf<File?>(null) }
    val ctx = LocalContext.current
    val files by s.recordings.collectAsStateWithLifecycle()
    val rec by s.rec.collectAsStateWithLifecycle()
    confirm?.let { f ->
        AlertDialog(
            onDismissRequest = { confirm = null },
            title = { Text("Delete recording?") },
            text = { Text(f.name) },
            confirmButton = { TextButton({ s.delete(f); confirm = null }) { Text("Delete") } },
            dismissButton = { TextButton({ confirm = null }) { Text("Cancel") } },
        )
    }
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp)) {
            Text("Recordings", style = MaterialTheme.typography.titleMedium)
            if (files.isEmpty()) Text("None yet", style = MaterialTheme.typography.bodySmall)
            files.forEach { f ->
                val busy = rec.active && f.name == rec.file
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(f.name, style = MaterialTheme.typography.bodyMedium)
                        Text("${f.length() / 1024} kB", style = MaterialTheme.typography.bodySmall)
                    }
                    TextButton({ share(ctx, f) }, Modifier.heightIn(min = TouchHeight), enabled = !busy) { Text("Share") }
                    TextButton({ confirm = f }, Modifier.heightIn(min = TouchHeight), enabled = !busy) { Text("Delete") }
                }
            }
        }
    }
}

private fun share(ctx: Context, f: File) {
    val uri = FileProvider.getUriForFile(ctx, "${ctx.packageName}.files", f)
    val send = Intent(Intent.ACTION_SEND)
        .setType("text/csv")
        .putExtra(Intent.EXTRA_STREAM, uri)
        .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
    ctx.startActivity(Intent.createChooser(send, null))
}
