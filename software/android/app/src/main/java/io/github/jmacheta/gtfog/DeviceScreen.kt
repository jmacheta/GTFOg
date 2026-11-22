package io.github.jmacheta.gtfog

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle

private val TABS = listOf("Control", "Settings", "Debug")

fun Conn.text() = when (this) {
    Conn.Connecting -> "Connecting…"
    Conn.Connected -> "Connected"
    Conn.Reconnecting -> "Out of range — reconnecting…"
    else -> "Disconnected"
}

@Composable
fun DeviceScreen(s: Session, st: DeviceState) {
    var tab by rememberSaveable { mutableIntStateOf(0) }
    var confirmDisconnect by rememberSaveable { mutableStateOf(false) }
    val dfu by s.dfu.state.collectAsStateWithLifecycle()
    val rec by s.rec.collectAsStateWithLifecycle()
    val snackbar = remember { SnackbarHostState() }
    // Leaving mid-update is not allowed (Back, Disconnect); on another tab Back goes to Control first.
    BackHandler(dfu.running || tab != 0) { if (!dfu.running) tab = 0 }
    // Keyed on the counter: the same failure again while the snackbar is up shows (and is cleared) as a new notice.
    LaunchedEffect(st.noticeSeq) {
        st.notice?.let { n ->
            snackbar.showSnackbar(n)
            s.client.clearNotice()
        }
    }
    if (confirmDisconnect) {
        AlertDialog(
            onDismissRequest = { confirmDisconnect = false },
            title = { Text("Stop recording and disconnect?") },
            text = { Text("The recording ${rec.file} ends now.") },
            confirmButton = { TextButton({ confirmDisconnect = false; s.disconnect() }) { Text("Disconnect") } },
            dismissButton = { TextButton({ confirmDisconnect = false }) { Text("Keep recording") } },
        )
    }
    // The Debug tab (recordings) and a running firmware update stay usable while the link is down.
    val offline = st.conn != Conn.Connected && tab != 2 && !(tab == 1 && dfu.running)
    Scaffold(
        topBar = {
            // The Scaffold insets cover the bottom bar and the content; the custom top bar takes the status bar and cutouts itself.
            Row(
                Modifier.windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal)).padding(16.dp, 8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(Modifier.weight(1f)) {
                    Text(st.name, style = MaterialTheme.typography.headlineSmall)
                    Text(st.conn.text(), Modifier.semantics { liveRegion = LiveRegionMode.Polite }, style = MaterialTheme.typography.bodySmall)
                }
                OutlinedButton(
                    { if (rec.active) confirmDisconnect = true else s.disconnect() },
                    Modifier.heightIn(min = TouchHeight), enabled = !dfu.running,
                ) { Text("Disconnect") }
            }
        },
        bottomBar = {
            NavigationBar {
                TABS.forEachIndexed { i, n ->
                    NavigationBarItem(tab == i, { tab = i }, { TabIcon(i) }, label = { Text(n) })
                }
            }
        },
        snackbarHost = { SnackbarHost(snackbar) },
    ) { pad ->
        Column(
            Modifier.padding(pad).verticalScroll(rememberScrollState()).padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            when {
                offline -> Offline(s, st, dfu.running)
                tab == 0 -> ControlTab(s, st)
                tab == 1 -> SettingsTab(s, st)
                else -> DebugTab(s, st)
            }
        }
    }
}

@Composable
private fun Offline(s: Session, st: DeviceState, updating: Boolean) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            when (st.conn) {
                Conn.Disconnected -> {
                    Text(st.error ?: "Disconnected", Modifier.semantics { liveRegion = LiveRegionMode.Polite }, style = MaterialTheme.typography.titleLarge)
                    Text("The goggles are not connected.")
                    Button({ s.client.reconnect() }, Modifier.fillMaxWidth().heightIn(min = MainHeight), enabled = !updating) { Text("Reconnect") }
                }
                else -> {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                        CircularProgressIndicator(Modifier.size(32.dp))
                        Text(st.conn.text(), Modifier.semantics { liveRegion = LiveRegionMode.Polite }, style = MaterialTheme.typography.titleLarge)
                    }
                    Text("Controls come back when the goggles answer.")
                }
            }
        }
    }
}

// Tiny Canvas icons so the app needs no icon library.
@Composable
private fun TabIcon(i: Int) { // decorative: the label next to it is read
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
