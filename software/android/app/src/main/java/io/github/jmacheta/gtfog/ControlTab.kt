package io.github.jmacheta.gtfog

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.selection.toggleable
import androidx.compose.material3.Card
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import kotlin.math.roundToInt

private val DELAYS = listOf("15 s" to 15, "30 s" to 30, "1 min" to 60, "2 min" to 120, "4 min" to 240)
private val LEVEL_SPEEDS = listOf("Stop" to 0) + (1..10).map { "${it * 10} %" to it * 10 }

@Composable
fun ControlTab(s: Session, st: DeviceState) {
    if (st.settings?.transportLock == true) {
        Card(Modifier.fillMaxWidth()) {
            Text(
                "Transport lock is on: the fan and strobe stay off. Unlock on the goggles: press + three times quickly.",
                Modifier.padding(16.dp), color = MaterialTheme.colorScheme.error,
            )
        }
    }
    BatteryWidget(st.battery, st.charger)
    FanCard(s, st)
    StrobeCard(s, st)
    ModeCard(s, st)
}

@Composable
private fun StrobeCard(s: Session, st: DeviceState) {
    Card(Modifier.fillMaxWidth()) {
        Row(
            Modifier.fillMaxWidth().heightIn(min = MainHeight)
                .toggleable(st.strobe == true, st.strobe != null, Role.Switch) { s.client.setStrobe(it) }
                .padding(16.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("Strobe", Modifier.weight(1f), style = MaterialTheme.typography.titleLarge)
            Switch(st.strobe == true, null, enabled = st.strobe != null)
        }
    }
}

@Composable
private fun ModeCard(s: Session, st: DeviceState) {
    val cur = st.settings
    val ok = cur != null && cur.extended
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Mode", style = MaterialTheme.typography.titleLarge)
            SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth()) {
                SegmentedButton(
                    cur?.motion != true, { s.client.updateSettings { it.copy(motion = false) } },
                    SegmentedButtonDefaults.itemShape(0, 2), Modifier.heightIn(min = MainHeight), enabled = ok,
                ) { Text("Manual") }
                SegmentedButton(
                    cur?.motion == true, { s.client.updateSettings { it.copy(motion = true) } },
                    SegmentedButtonDefaults.itemShape(1, 2), Modifier.heightIn(min = MainHeight), enabled = ok,
                ) { Text("Motion") }
            }
            if (cur == null || !cur.extended) {
                Text("Update the firmware to use the motion mode.", style = MaterialTheme.typography.bodySmall)
            } else if (cur.motion) {
                Text("Slow down after", style = MaterialTheme.typography.titleMedium)
                ChoiceChips(DELAYS, cur.motionDelayS) { v -> s.client.updateSettings { it.copy(motionDelayS = v) } }
                if (cur.perActivity) {
                    ACTIVITY_NAMES.forEachIndexed { level, name ->
                        Text(name, style = MaterialTheme.typography.titleMedium)
                        ChoiceChips(LEVEL_SPEEDS, cur.motionSpeeds[level]) { v ->
                            s.client.updateSettings { it.copy(motionSpeeds = it.motionSpeeds.toMutableList().also { l -> l[level] = v }) }
                        }
                    }
                    Text(
                        "The fan follows how much you move. The - and + buttons change the speed of the current activity.",
                        style = MaterialTheme.typography.bodySmall,
                    )
                } else {
                    Text("Update the firmware to set a speed for each activity.", style = MaterialTheme.typography.bodySmall)
                }
            }
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
    val description = batteryDescription(level, charger)
    Card(Modifier.fillMaxWidth()) {
        Row(
            Modifier.padding(16.dp).clearAndSetSemantics { contentDescription = description },
            verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(16.dp),
        ) {
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
private fun FanCard(s: Session, st: DeviceState) {
    var drag by remember { mutableStateOf<Float?>(null) }
    val target = st.fan ?: 0
    // The big number is what the fan runs at; in motion mode the target is the one of the current activity.
    val actual = st.fanActual ?: target
    val shown = drag ?: actual.toFloat()
    val enabled = st.fan != null
    val step = st.settings?.fanStep ?: 5
    val button = Modifier.heightIn(min = MainHeight).widthIn(min = 88.dp)
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp)) {
            Text("Fan", style = MaterialTheme.typography.titleLarge)
            Text("${shown.roundToInt()} %", Modifier.fillMaxWidth(), style = MaterialTheme.typography.displayLarge, textAlign = TextAlign.Center)
            val level = if (st.settings?.motion == true) st.activity else null
            if (level != null) {
                Text("Activity: ${ACTIVITY_NAMES[level]}", Modifier.fillMaxWidth(), style = MaterialTheme.typography.titleMedium, textAlign = TextAlign.Center)
            }
            if (drag == null && actual != target) {
                Text("Target $target %", Modifier.fillMaxWidth(), style = MaterialTheme.typography.bodyMedium, textAlign = TextAlign.Center)
            }
            Row(verticalAlignment = Alignment.CenterVertically) {
                FilledTonalButton({ s.client.stepFan(-1) }, button, enabled = enabled) { Text("−$step %", style = MaterialTheme.typography.titleLarge) }
                Slider(
                    value = drag ?: target.toFloat(),
                    onValueChange = { drag = it },
                    onValueChangeFinished = { drag?.let { s.client.setFan(it.roundToInt()) }; drag = null },
                    valueRange = 0f..100f,
                    enabled = enabled,
                    modifier = Modifier.weight(1f).padding(horizontal = 8.dp),
                )
                FilledTonalButton({ s.client.stepFan(1) }, button, enabled = enabled) { Text("+$step %", style = MaterialTheme.typography.titleLarge) }
            }
        }
    }
}
