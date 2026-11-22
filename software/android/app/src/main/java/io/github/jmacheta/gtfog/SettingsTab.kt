package io.github.jmacheta.gtfog

import android.provider.OpenableColumns
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.selection.toggleable
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch

private val AUTO_OFF = listOf("Off" to 0, "5 min" to 5, "10 min" to 10, "15 min" to 15, "30 min" to 30, "60 min" to 60)
private val FAN_STEPS = listOf("1 %" to 1, "2 %" to 2, "5 %" to 5, "10 %" to 10, "20 %" to 20)

@Composable
fun SettingsTab(s: Session, st: DeviceState) {
    val cur = st.settings
    val ok = st.hasSettings && cur != null
    if (!st.hasSettings) Text("Update the firmware to change settings", color = MaterialTheme.colorScheme.error)
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            SettingSwitch("Stealth mode", "Status LED never lights up", cur?.stealth == true, ok) { v ->
                s.client.updateSettings { it.copy(stealth = v) }
            }
            SettingSwitch(
                "Transport lock",
                "Buttons only wake the device; fan and strobe stay off. Unlock on the goggles: press + three times quickly.",
                cur?.transportLock == true, ok,
            ) { v -> s.client.updateSettings { it.copy(transportLock = v) } }
            Column {
                Text("Auto shutdown", style = MaterialTheme.typography.titleMedium)
                Text(
                    "Power off after this long with the fan and strobe off and no phone connected",
                    style = MaterialTheme.typography.bodySmall,
                )
                ChoiceChips(AUTO_OFF, cur?.autoOffMin, ok) { v -> s.client.updateSettings { it.copy(autoOffMin = v) } }
            }
            Column {
                Text("Fan step per click", style = MaterialTheme.typography.titleMedium)
                Text(
                    if (cur != null && !cur.extended) "Update the firmware to change the step." else "How much a click of + or - on the goggles changes the fan.",
                    style = MaterialTheme.typography.bodySmall,
                )
                ChoiceChips(FAN_STEPS, cur?.fanStep, ok && cur?.extended == true) { v -> s.client.updateSettings { it.copy(fanStep = v) } }
            }
        }
    }
    FirmwareCard(s, st)
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp)) {
            Text("Device", style = MaterialTheme.typography.titleMedium)
            Text("Manufacturer: ${st.manufacturer ?: "-"}")
            Text("Model: ${st.model ?: "-"}")
            Text("New phones can pair only within 3 minutes after the goggles are switched on.", style = MaterialTheme.typography.bodySmall)
        }
    }
}

@Composable
private fun FirmwareCard(s: Session, st: DeviceState) {
    val d by s.dfu.state.collectAsStateWithLifecycle()
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()
    val connected = st.conn == Conn.Connected
    val block = dfuBlockReason(st.battery)
    // Not right after an update: Dfu reads the version itself once the goggles are back.
    LaunchedEffect(connected, d.installed == null, d.done) { if (connected && d.installed == null && !d.done) s.dfu.refresh() }
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) scope.launch(Dispatchers.IO) { // up to 4 MB: not on the main thread
            var name = uri.lastPathSegment ?: "file"
            ctx.contentResolver.query(uri, null, null, null, null)?.use { c ->
                if (c.moveToFirst()) {
                    c.getColumnIndex(OpenableColumns.DISPLAY_NAME).takeIf { it >= 0 }?.let { name = c.getString(it) }
                }
            }
            // The provider's size column may be missing or wrong: the read itself is capped.
            val r = runCatching { requireNotNull(ctx.contentResolver.openInputStream(uri)).use { it.readCapped(MAX_IMAGE) } }
            s.dfu.pick(name, r.getOrNull(), r.exceptionOrNull()?.takeIf { it is IllegalArgumentException && it.message?.startsWith("File too large") == true }?.message ?: "Cannot read the file")
        }
    }
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Firmware", style = MaterialTheme.typography.titleMedium)
            Text(
                "Installed version: " + (d.installed ?: if (d.done) "reading from the goggles…" else "-") +
                    (if (d.installed == null && d.refreshError != null) " (could not read: ${d.refreshError})" else ""),
            )
            if (d.file != null) Text("File: ${d.file}" + (d.fileVersion?.let { " (version $it)" } ?: ""))
            versionHint(d.installed, d.fileVersion)?.let { Text(it, style = MaterialTheme.typography.bodySmall) }
            if (d.image != null && !d.running && block != null) Text(block, color = MaterialTheme.colorScheme.error)
            if (d.running) {
                Text("Keep the app open and the phone near the goggles until the update is done. Live accelerometer and recording are stopped during the update.")
                LinearProgressIndicator(progress = { d.percent / 100f }, Modifier.fillMaxWidth())
                Text("${d.status} ${d.percent}%")
                if (d.status == "Uploading") OutlinedButton({ s.dfu.cancel() }, Modifier.heightIn(min = TouchHeight)) { Text("Cancel") }
            } else {
                d.error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
                d.detail?.let { Text(it, style = MaterialTheme.typography.bodySmall) }
                if (d.done) Text("Update installed and confirmed")
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton({ picker.launch(arrayOf("*/*")) }, Modifier.heightIn(min = TouchHeight), enabled = connected) { Text("Update firmware…") }
                    if (d.image != null) Button({ s.dfu.install() }, Modifier.heightIn(min = TouchHeight), enabled = connected && !d.done && block == null) { Text("Install") }
                }
            }
        }
    }
}

@Composable
private fun SettingSwitch(title: String, subtitle: String, on: Boolean, enabled: Boolean, onChange: (Boolean) -> Unit) {
    Row(
        Modifier.toggleable(on, enabled, Role.Switch, onValueChange = onChange).heightIn(min = TouchHeight),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f).padding(end = 12.dp)) {
            Text(title, style = MaterialTheme.typography.titleMedium)
            Text(subtitle, style = MaterialTheme.typography.bodySmall)
        }
        Switch(on, null, enabled = enabled)
    }
}
