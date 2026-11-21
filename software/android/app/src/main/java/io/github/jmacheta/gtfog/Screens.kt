@file:SuppressLint("MissingPermission")

package io.github.jmacheta.gtfog

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import androidx.core.content.FileProvider
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.delay
import java.io.File
import java.util.Locale
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.roundToInt

private val blePermissions: Array<String> =
    if (Build.VERSION.SDK_INT >= 31) arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
    else arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)

private fun hasPermissions(ctx: Context) =
    blePermissions.all { ContextCompat.checkSelfPermission(ctx, it) == PackageManager.PERMISSION_GRANTED }

@Composable
fun ScanScreen(s: Session, st: DeviceState) {
    val ctx = LocalContext.current
    var granted by remember { mutableStateOf(hasPermissions(ctx)) }
    var btOn by remember { mutableStateOf(s.adapter?.isEnabled == true) }
    val askPerms = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        granted = hasPermissions(ctx)
    }
    val enableBt = rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) {}
    val items by s.scanResults.collectAsStateWithLifecycle()

    LaunchedEffect(Unit) { if (!granted) askPerms.launch(blePermissions) }
    DisposableEffect(Unit) {
        val rx = object : BroadcastReceiver() {
            override fun onReceive(c: Context, i: Intent) {
                btOn = s.adapter?.isEnabled == true
            }
        }
        ContextCompat.registerReceiver(ctx, rx, IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED), ContextCompat.RECEIVER_NOT_EXPORTED)
        onDispose { ctx.unregisterReceiver(rx) }
    }
    DisposableEffect(granted, btOn) {
        if (granted && btOn) s.startScan()
        onDispose { if (granted) s.stopScan() }
    }

    Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Text("GTFOg", style = MaterialTheme.typography.headlineMedium)
        st.error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
        when {
            !granted -> {
                Text("Bluetooth permission is needed to find the device.")
                Button({ askPerms.launch(blePermissions) }) { Text("Grant permission") }
            }
            !btOn -> {
                Text("Bluetooth is off.")
                Button({ enableBt.launch(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE)) }) { Text("Enable Bluetooth") }
            }
            else -> {
                Text(if (items.isEmpty()) "Scanning..." else "Tap a device to connect")
                LazyColumn(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    items(items, key = { it.address }) {
                        Card(Modifier.fillMaxWidth().clickable { s.connect(it) }) {
                            Row(Modifier.padding(16.dp), horizontalArrangement = Arrangement.SpaceBetween) {
                                Column(Modifier.weight(1f)) {
                                    Text(it.name, style = MaterialTheme.typography.titleMedium)
                                    Text(it.address, style = MaterialTheme.typography.bodySmall)
                                }
                                Text("${it.rssi} dBm")
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
fun DeviceScreen(s: Session, st: DeviceState) {
    Column(
        Modifier.verticalScroll(rememberScrollState()).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(st.name, style = MaterialTheme.typography.headlineSmall)
                Text("${st.conn}  ${st.address}", style = MaterialTheme.typography.bodySmall)
            }
            OutlinedButton({ s.disconnect() }) { Text("Disconnect") }
        }
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp)) {
                Text("Battery: ${st.battery?.let { "$it %" } ?: "-"}")
                Text("Charger: ${st.charger?.let { "${chargerText(it)} (raw $it)" } ?: "-"}")
                Text("${st.manufacturer ?: "-"} / ${st.model ?: "-"}", style = MaterialTheme.typography.bodySmall)
            }
        }
        FanCard(s, st)
        Card(Modifier.fillMaxWidth()) {
            Row(Modifier.padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
                Text("Strobe", Modifier.weight(1f))
                Switch(st.strobe == true, { s.client.setStrobe(it) }, enabled = st.strobe != null)
            }
        }
        AccelCard(s, st)
        Recordings(s)
    }
}

@Composable
private fun FanCard(s: Session, st: DeviceState) {
    var drag by remember { mutableStateOf<Float?>(null) }
    val shown = drag ?: (st.fan ?: 0).toFloat()
    val enabled = st.fan != null
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp)) {
            Text("Fan speed: ${shown.roundToInt()} %")
            Row(verticalAlignment = Alignment.CenterVertically) {
                OutlinedButton({ s.client.setFan(((st.fan ?: 0) - 5).coerceAtLeast(0)) }, enabled = enabled) { Text("-") }
                Slider(
                    value = shown,
                    onValueChange = { drag = it },
                    onValueChangeFinished = { drag?.let { s.client.setFan(it.roundToInt()) }; drag = null },
                    valueRange = 0f..100f,
                    enabled = enabled,
                    modifier = Modifier.weight(1f).padding(horizontal = 8.dp),
                )
                OutlinedButton({ s.client.setFan(((st.fan ?: 0) + 5).coerceAtMost(100)) }, enabled = enabled) { Text("+") }
            }
        }
    }
}

private val AXIS_COLORS = listOf(Color(0xFFE53935), Color(0xFF43A047), Color(0xFF1E88E5))
private val AXIS_NAMES = listOf("x", "y", "z")
private val WINDOWS = listOf(5, 10, 30)
private val RANGES = listOf(null, 2.0, 4.0, 8.0, 16.0)
private val GRID_STEPS = doubleArrayOf(0.1, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0)

@Composable
private fun AccelCard(s: Session, st: DeviceState) {
    val ctx = LocalContext.current
    val stats by s.stats.collectAsStateWithLifecycle()
    val rec by s.rec.collectAsStateWithLifecycle()
    var windowS by remember { mutableStateOf(10) }
    var range by remember { mutableStateOf<Double?>(null) }
    var tick by remember { mutableLongStateOf(0) }
    var nowSec by remember { mutableLongStateOf(0) }
    var mag by remember { mutableStateOf(0.0) }
    var rate by remember { mutableStateOf(0.0) }
    val meter = remember { RateMeter() }
    val askNotif = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { s.startRecording() }

    LaunchedEffect(Unit) {
        while (true) {
            delay(33)
            val now = System.currentTimeMillis()
            tick = now
            nowSec = now / 1000
            mag = Math.round((s.ring.last()?.let(::magnitudeG) ?: 0.0) * 100) / 100.0
            rate = meter.update(now, s.stats.value.samples)
        }
    }

    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Accelerometer", Modifier.weight(1f), style = MaterialTheme.typography.titleMedium)
                Text("Live  ")
                Switch(st.live, { s.setLive(it) }, enabled = st.conn == Conn.Connected)
            }
            Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                WINDOWS.forEach { FilterChip(windowS == it, { windowS = it }, { Text("${it} s") }) }
            }
            Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                RANGES.forEach { FilterChip(range == it, { range = it }, { Text(it?.let { r -> "±${r.toInt()} g" } ?: "auto") }) }
            }
            AccelChart(s.ring, windowS, range, tick)
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
                AXIS_NAMES.forEachIndexed { i, n ->
                    Box(AXIS_COLORS[i]); Text(n)
                }
            }
            Text(
                String.format(Locale.US, "|a| %.2f g   %.0f samples/s   lost frames %d", mag, rate, stats.lost),
                style = MaterialTheme.typography.bodyMedium,
            )
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                if (rec.active) {
                    Button({ s.stopRecording() }) { Text("Stop") }
                } else {
                    Button(
                        {
                            if (Build.VERSION.SDK_INT >= 33 &&
                                ContextCompat.checkSelfPermission(ctx, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
                            ) askNotif.launch(Manifest.permission.POST_NOTIFICATIONS) else s.startRecording()
                        },
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
private fun Box(color: Color) {
    Row(Modifier.size(12.dp).background(color)) {}
}

@Composable
private fun AccelChart(ring: SampleRing, windowS: Int, rangeG: Double?, tick: Long) {
    val bufs = remember { Array(3) { IntArray(3000) } }
    val measurer = rememberTextMeasurer()
    val gridColor = MaterialTheme.colorScheme.outlineVariant
    val textStyle = TextStyle(fontSize = 10.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
    Canvas(Modifier.fillMaxWidth().height(240.dp)) {
        tick // redraw trigger, set at most every 33 ms
        val n = windowS * 100
        val m = ring.snapshot(n, bufs)
        var peak = 0
        for (b in bufs) for (k in 0 until m) peak = max(peak, abs(b[k]))
        val r = rangeG ?: autoRangeG(peak / 1000.0)
        val left = 40.dp.toPx()
        val bottom = 16.dp.toPx()
        val w = size.width - left
        val h = size.height - bottom
        fun yOf(g: Double) = ((r - g) / (2 * r) * h).toFloat()

        val step = GRID_STEPS.firstOrNull { 2 * r / it <= 8 } ?: 8.0
        var g = -r
        while (g <= r + 1e-9) {
            val y = yOf(g)
            drawLine(gridColor, Offset(left, y), Offset(size.width, y), 1f)
            val label = measurer.measure(java.math.BigDecimal(g).setScale(2, java.math.RoundingMode.HALF_UP).stripTrailingZeros().toPlainString(), textStyle)
            drawText(label, topLeft = Offset(left - label.size.width - 4.dp.toPx(), y - label.size.height / 2f))
            g += step
        }
        val every = if (windowS == 5) 1 else if (windowS == 10) 2 else 5
        for (t in 0..windowS step every) {
            val x = left + w - w * t / windowS
            drawLine(gridColor, Offset(x, 0f), Offset(x, h), 1f)
            val label = measurer.measure(if (t == 0) "0 s" else "-$t s", textStyle)
            drawText(label, topLeft = Offset(x - label.size.width / 2f, h + 2.dp.toPx()))
        }
        val dx = w / (n - 1)
        clipRect(left, 0f, size.width, h) {
            for (a in 0..2) {
                val p = Path()
                for (k in 0 until m) {
                    val x = left + w - (m - 1 - k) * dx
                    val y = yOf(bufs[a][k] / 1000.0)
                    if (k == 0) p.moveTo(x, y) else p.lineTo(x, y)
                }
                drawPath(p, AXIS_COLORS[a], style = Stroke(1.5.dp.toPx()))
            }
        }
    }
}

@Composable
private fun Recordings(s: Session) {
    val ctx = LocalContext.current
    val files by s.recordings.collectAsStateWithLifecycle()
    val rec by s.rec.collectAsStateWithLifecycle()
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
                    TextButton({ share(ctx, f) }, enabled = !busy) { Text("Share") }
                    TextButton({ s.delete(f) }, enabled = !busy) { Text("Delete") }
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
