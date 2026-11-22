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
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.text.style.TextAlign
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

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun DeviceScreen(s: Session, st: DeviceState) {
    var tab by rememberSaveable { mutableIntStateOf(0) }
    Scaffold(
        topBar = {
            Row(Modifier.padding(16.dp, 8.dp), verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(st.name, style = MaterialTheme.typography.headlineSmall)
                    Text("${st.conn}  ${st.address}", style = MaterialTheme.typography.bodySmall)
                }
                OutlinedButton({ s.disconnect() }) { Text("Disconnect") }
            }
        },
        bottomBar = {
            NavigationBar(windowInsets = WindowInsets(0)) {
                listOf("Control", "Settings", "Debug").forEachIndexed { i, n ->
                    NavigationBarItem(tab == i, { tab = i }, { TabIcon(i) }, label = { Text(n) })
                }
            }
        },
        contentWindowInsets = WindowInsets(0),
    ) { pad ->
        Column(
            Modifier.padding(pad).verticalScroll(rememberScrollState()).padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            when (tab) {
                0 -> ControlTab(s, st)
                1 -> SettingsTab(s, st)
                else -> {
                    AccelCard(s, st)
                    Recordings(s)
                }
            }
        }
    }
}

// Tiny Canvas icons so the app needs no icon library.
@Composable
private fun TabIcon(i: Int) {
    val c = LocalContentColor.current
    Canvas(Modifier.size(24.dp)) {
        val st = Stroke(2.dp.toPx())
        val w = size.width
        when (i) {
            0 -> { // fan: circle and a hub
                drawCircle(c, w * 0.42f, style = st)
                drawCircle(c, w * 0.1f)
            }
            1 -> { // sliders
                for (k in 0..2) {
                    val y = w * (0.2f + 0.3f * k)
                    drawLine(c, Offset(w * 0.1f, y), Offset(w * 0.9f, y), 2.dp.toPx())
                    drawCircle(c, w * 0.1f, Offset(w * (0.3f + 0.2f * k), y))
                }
            }
            else -> { // waveform
                val p = Path()
                p.moveTo(w * 0.05f, w * 0.5f)
                p.lineTo(w * 0.3f, w * 0.15f); p.lineTo(w * 0.55f, w * 0.85f); p.lineTo(w * 0.95f, w * 0.4f)
                drawPath(p, c, style = st)
            }
        }
    }
}

@Composable
private fun ControlTab(s: Session, st: DeviceState) {
    BatteryWidget(st.battery, st.charger)
    FanCard(s, st)
    Card(Modifier.fillMaxWidth()) {
        Row(Modifier.padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
            Text("Strobe", Modifier.weight(1f), style = MaterialTheme.typography.titleLarge)
            Switch(st.strobe == true, { s.client.setStrobe(it) }, enabled = st.strobe != null)
        }
    }
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Mode", style = MaterialTheme.typography.titleLarge)
            SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth()) {
                SegmentedButton(true, {}, SegmentedButtonDefaults.itemShape(0, 2)) { Text("Manual") }
                SegmentedButton(false, {}, SegmentedButtonDefaults.itemShape(1, 2), enabled = false) { Text("Motion") }
            }
            Text("Motion: coming soon", style = MaterialTheme.typography.bodySmall)
        }
    }
}

private val GREEN = Color(0xFF43A047)
private val RED = Color(0xFFE53935)

@Composable
private fun BatteryWidget(level: Int?, charger: Int?) {
    val fault = charger == 3
    val grey = MaterialTheme.colorScheme.outline
    val fg = MaterialTheme.colorScheme.onSurface
    val color = when {
        fault -> RED
        level == null -> grey
        level <= 15 -> RED
        else -> GREEN
    }
    Card(Modifier.fillMaxWidth()) {
        Row(Modifier.padding(16.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(16.dp)) {
            Canvas(Modifier.size(96.dp, 48.dp)) {
                val tip = size.width * 0.07f
                val bw = size.width - tip
                val sw = 3.dp.toPx()
                drawRoundRect(color, Offset(sw / 2, sw / 2), Size(bw - sw, size.height - sw), CornerRadius(6.dp.toPx()), Stroke(sw))
                drawRect(color, Offset(bw, size.height * 0.3f), Size(tip, size.height * 0.4f))
                if (level != null && !fault) {
                    val f = level.coerceIn(0, 100) / 100f
                    val inset = sw + 2.dp.toPx()
                    drawRect(color, Offset(inset, inset), Size((bw - 2 * inset) * f, size.height - 2 * inset))
                }
                if (fault || charger == 1) {
                    val cx = bw / 2
                    val h = size.height
                    val sym = if (fault) Color.White else Color.Black
                    if (fault) {
                        drawRoundRect(Color.Black, Offset(cx - 4.dp.toPx(), h * 0.2f), Size(8.dp.toPx(), h * 0.38f), CornerRadius(2.dp.toPx()))
                        drawCircle(Color.Black, 4.dp.toPx(), Offset(cx, h * 0.76f))
                        drawRoundRect(sym, Offset(cx - 2.5.dp.toPx(), h * 0.24f), Size(5.dp.toPx(), h * 0.3f), CornerRadius(2.dp.toPx()))
                        drawCircle(sym, 2.5.dp.toPx(), Offset(cx, h * 0.76f))
                    } else {
                        val p = Path()
                        p.moveTo(cx + 3.dp.toPx(), h * 0.1f)
                        p.lineTo(cx - 9.dp.toPx(), h * 0.55f); p.lineTo(cx - 1.dp.toPx(), h * 0.55f)
                        p.lineTo(cx - 3.dp.toPx(), h * 0.9f)
                        p.lineTo(cx + 9.dp.toPx(), h * 0.43f); p.lineTo(cx + 1.dp.toPx(), h * 0.43f)
                        p.close()
                        drawPath(p, Color.White)
                        drawPath(p, Color.Black, style = Stroke(1.5.dp.toPx()))
                    }
                }
            }
            Column {
                Text(level?.let { "$it %" } ?: "-", style = MaterialTheme.typography.displaySmall, color = fg)
                when {
                    fault -> Text("Charger fault", color = RED, style = MaterialTheme.typography.titleMedium)
                    charger == 1 -> Text("Charging", style = MaterialTheme.typography.titleMedium)
                }
            }
        }
    }
}

@Composable
private fun SettingsTab(s: Session, st: DeviceState) {
    val cur = st.settings
    val ok = st.hasSettings && cur != null
    if (!st.hasSettings) Text("Update the firmware to change settings", color = MaterialTheme.colorScheme.error)
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            SettingSwitch("Stealth mode", "Status LED never lights up", cur?.stealth == true, ok) {
                cur?.let { c -> s.client.setSettings(c.copy(stealth = it)) }
            }
            SettingSwitch("Transport lock", "Buttons only wake the device; fan and strobe stay off", cur?.transportLock == true, ok) {
                cur?.let { c -> s.client.setSettings(c.copy(transportLock = it)) }
            }
            Column {
                Text("Auto shutdown", style = MaterialTheme.typography.titleMedium)
                Text(
                    "Power off after this long with the fan and strobe off and no phone connected",
                    style = MaterialTheme.typography.bodySmall,
                )
                Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    AUTO_OFF.forEach { m ->
                        FilterChip(cur?.autoOffMin == m, { cur?.let { s.client.setSettings(it.copy(autoOffMin = m)) } }, { Text(if (m == 0) "Off" else "$m min") }, enabled = ok)
                    }
                }
            }
        }
    }
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp)) {
            Text("Device", style = MaterialTheme.typography.titleMedium)
            Text("Manufacturer: ${st.manufacturer ?: "-"}")
            Text("Model: ${st.model ?: "-"}")
        }
    }
}

private val AUTO_OFF = listOf(0, 5, 10, 15, 30, 60)

@Composable
private fun SettingSwitch(title: String, subtitle: String, on: Boolean, enabled: Boolean, onChange: (Boolean) -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f).padding(end = 12.dp)) {
            Text(title, style = MaterialTheme.typography.titleMedium)
            Text(subtitle, style = MaterialTheme.typography.bodySmall)
        }
        Switch(on, onChange, enabled = enabled)
    }
}

@Composable
private fun FanCard(s: Session, st: DeviceState) {
    var drag by remember { mutableStateOf<Float?>(null) }
    val shown = drag ?: (st.fan ?: 0).toFloat()
    val enabled = st.fan != null
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp)) {
            Text("Fan", style = MaterialTheme.typography.titleLarge)
            Text("${shown.roundToInt()} %", Modifier.fillMaxWidth(), style = MaterialTheme.typography.displayLarge, textAlign = TextAlign.Center)
            Row(verticalAlignment = Alignment.CenterVertically) {
                FilledTonalButton({ s.client.setFan(((st.fan ?: 0) - 5).coerceAtLeast(0)) }, enabled = enabled) { Text("-5", style = MaterialTheme.typography.titleLarge) }
                Slider(
                    value = shown,
                    onValueChange = { drag = it },
                    onValueChangeFinished = { drag?.let { s.client.setFan(it.roundToInt()) }; drag = null },
                    valueRange = 0f..100f,
                    enabled = enabled,
                    modifier = Modifier.weight(1f).padding(horizontal = 8.dp),
                )
                FilledTonalButton({ s.client.setFan(((st.fan ?: 0) + 5).coerceAtMost(100)) }, enabled = enabled) { Text("+5", style = MaterialTheme.typography.titleLarge) }
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
