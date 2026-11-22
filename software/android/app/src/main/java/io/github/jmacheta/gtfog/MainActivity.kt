package io.github.jmacheta.gtfog

import android.os.Bundle
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.Surface
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.lifecycle.compose.collectAsStateWithLifecycle

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        val session = (application as GtfogApp).session
        setContent {
            GtfogTheme {
                Surface(Modifier.fillMaxSize()) {
                    Box(Modifier.fillMaxSize()) {
                        val st by session.client.state.collectAsStateWithLifecycle()
                        val updating by session.dfu.state.collectAsStateWithLifecycle()
                        // The screen stays on for the whole update, whichever tab is shown.
                        DisposableEffect(updating.running) {
                            if (updating.running) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                            onDispose { window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON) }
                        }
                        if (st.conn == Conn.Idle) ScanScreen(session, st) else DeviceScreen(session, st)
                    }
                }
            }
        }
    }

    // Back on the Control tab ends the activity: drop the link unless a recording needs it (its service keeps the process) or an update runs.
    override fun onDestroy() {
        super.onDestroy()
        val s = (application as GtfogApp).session
        if (isFinishing && !s.rec.value.active && !s.dfu.state.value.running) s.disconnect()
    }
}
