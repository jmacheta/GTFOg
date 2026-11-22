package io.github.jmacheta.gtfog

import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp

/** Height of the main controls (fan, strobe, mode, connect): easy to hit with gloves. */
val MainHeight = 56.dp

/** Minimum height of every other touch target. */
val TouchHeight = 48.dp

/** Follows the system light/dark setting; the stock M3 schemes keep the contrast high enough for daylight. */
@Composable
fun GtfogTheme(content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = if (isSystemInDarkTheme()) darkColorScheme() else lightColorScheme(), content = content)
}

/** One row of single-choice chips, scrollable when it does not fit. */
@Composable
fun <T> ChoiceChips(options: List<Pair<String, T>>, selected: T?, enabled: Boolean = true, onPick: (T) -> Unit) {
    Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        options.forEach { (label, v) ->
            FilterChip(selected == v, { onPick(v) }, { Text(label) }, Modifier.heightIn(min = TouchHeight), enabled = enabled)
        }
    }
}
