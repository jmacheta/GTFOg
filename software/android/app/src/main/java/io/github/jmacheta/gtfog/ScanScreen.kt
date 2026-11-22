@file:SuppressLint("MissingPermission")

package io.github.jmacheta.gtfog

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.bluetooth.BluetoothAdapter
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.location.LocationManager
import android.net.Uri
import android.os.Build
import android.provider.Settings
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.key
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.core.location.LocationManagerCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LifecycleEventEffect
import androidx.lifecycle.compose.collectAsStateWithLifecycle

val blePermissions: Array<String> =
    if (Build.VERSION.SDK_INT >= 31) arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
    else arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)

private fun hasPermissions(ctx: Context) =
    blePermissions.all { ContextCompat.checkSelfPermission(ctx, it) == PackageManager.PERMISSION_GRANTED }

// After a refused request: "do not ask again" (or refused twice on Android 11+) means no rationale is offered any more.
private fun permanentlyDenied(ctx: Context) =
    !hasPermissions(ctx) && blePermissions.any {
        ContextCompat.checkSelfPermission(ctx, it) != PackageManager.PERMISSION_GRANTED &&
            (ctx as? Activity)?.let { a -> !ActivityCompat.shouldShowRequestPermissionRationale(a, it) } == true
    }

/** Only Android 11 and older need the Location switch for BLE scanning (API 31+ has neverForLocation). */
private fun locationOn(ctx: Context) =
    Build.VERSION.SDK_INT >= 31 || ctx.getSystemService(LocationManager::class.java)?.let { LocationManagerCompat.isLocationEnabled(it) } != false

@Composable
fun ScanScreen(s: Session, st: DeviceState) {
    val ctx = LocalContext.current
    var granted by remember { mutableStateOf(hasPermissions(ctx)) }
    var denied by remember { mutableStateOf(false) }
    var btOn by remember { mutableStateOf(s.adapter?.isEnabled == true) }
    var locOn by remember { mutableStateOf(locationOn(ctx)) }
    val askPerms = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        granted = hasPermissions(ctx)
        denied = permanentlyDenied(ctx)
    }
    val enableBt = rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) {}
    val items by s.scanResults.collectAsStateWithLifecycle()
    val scanning by s.scanning.collectAsStateWithLifecycle()
    val scanError by s.scanError.collectAsStateWithLifecycle()

    LaunchedEffect(Unit) { if (!granted) askPerms.launch(blePermissions) }
    // Back from the system settings: pick up a permission granted there.
    LifecycleEventEffect(Lifecycle.Event.ON_RESUME) {
        granted = hasPermissions(ctx)
        btOn = s.adapter?.isEnabled == true
        locOn = locationOn(ctx)
        if (granted) denied = false
    }
    DisposableEffect(Unit) {
        val rx = object : BroadcastReceiver() {
            override fun onReceive(c: Context, i: Intent) {
                btOn = s.adapter?.isEnabled == true
            }
        }
        ContextCompat.registerReceiver(ctx, rx, IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED), ContextCompat.RECEIVER_NOT_EXPORTED)
        onDispose { ctx.unregisterReceiver(rx) }
    }
    DisposableEffect(granted, btOn, locOn) {
        if (granted && btOn) s.startScan()
        onDispose { if (granted) s.stopScan() }
    }

    // The whole page scrolls (large font, landscape); the few devices in range are plain rows, not a nested lazy list.
    Column(Modifier.safeDrawingPadding().verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Text("GTFOg", style = MaterialTheme.typography.headlineMedium)
        st.error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
        if (st.bondProblem) {
            Button({ ctx.startActivity(Intent(Settings.ACTION_BLUETOOTH_SETTINGS)) }, Modifier.heightIn(min = MainHeight)) { Text("Open Bluetooth settings") }
        }
        when {
            !granted -> {
                Text(
                    "GTFOg finds your goggles over Bluetooth. Android first asks for permission to scan for and connect to " +
                        "nearby devices. The first time you connect, Android shows a pairing dialog: accept it.",
                )
                if (denied) {
                    Text("The permission was refused. Allow it in the app settings.", color = MaterialTheme.colorScheme.error)
                    Button(
                        {
                            ctx.startActivity(
                                Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", ctx.packageName, null)),
                            )
                        },
                        Modifier.heightIn(min = MainHeight),
                    ) { Text("Open app settings") }
                } else {
                    Button({ askPerms.launch(blePermissions) }, Modifier.heightIn(min = MainHeight)) { Text("Grant permission") }
                }
            }
            !btOn -> {
                Text("Bluetooth is off.")
                Button({ enableBt.launch(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE)) }, Modifier.heightIn(min = MainHeight)) { Text("Enable Bluetooth") }
            }
            else -> {
                Text(
                    when {
                        scanError != null -> scanError!!
                        items.isNotEmpty() -> "Tap your goggles to connect"
                        scanning -> "Scanning…"
                        else -> "No goggles found. Switch them on and scan again."
                    },
                    color = if (scanError != null) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurface,
                )
                if (!locOn) {
                    // Android 11 and older find no Bluetooth devices while Location is off.
                    Text("Location is off. This Android version needs it to find Bluetooth devices.", color = MaterialTheme.colorScheme.error)
                    Button({ ctx.startActivity(Intent(Settings.ACTION_LOCATION_SOURCE_SETTINGS)) }, Modifier.heightIn(min = MainHeight)) { Text("Open Location settings") }
                }
                if (!scanning) Button({ s.startScan() }, Modifier.heightIn(min = MainHeight)) { Text("Scan again") }
                Text(
                    "On the first connection Android shows a pairing dialog: accept it.",
                    style = MaterialTheme.typography.bodySmall,
                )
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    items.forEach {
                        key(it.address) {
                            Card(Modifier.fillMaxWidth().heightIn(min = MainHeight).clickable(onClickLabel = "Connect", role = Role.Button) { s.connect(it) }) {
                                Row(Modifier.padding(16.dp), horizontalArrangement = Arrangement.SpaceBetween) {
                                    Column(Modifier.weight(1f)) {
                                        Text(it.label, style = MaterialTheme.typography.titleMedium)
                                        Text(
                                            it.address + if (it.address == s.lastAddress) "  (last used)" else "",
                                            style = MaterialTheme.typography.bodySmall,
                                        )
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
}
