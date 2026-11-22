package io.github.jmacheta.gtfog

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.size
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlin.math.abs
import kotlin.math.max

val AXIS_COLORS = listOf(Color(0xFFE53935), Color(0xFF43A047), Color(0xFF1E88E5))
val AXIS_NAMES = listOf("x", "y", "z")
val WINDOWS = listOf(5, 10, 30)
val RANGES = listOf(null, 2.0, 4.0, 8.0, 16.0)
private val GRID_STEPS = doubleArrayOf(0.1, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0)

@Composable
fun Swatch(color: Color) {
    Row(Modifier.size(12.dp).background(color)) {}
}

@Composable
fun AccelChart(ring: SampleRing, windowS: Int, rangeG: Double?, tick: Long) {
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
