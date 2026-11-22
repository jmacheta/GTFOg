# Android app

Kotlin / Jetpack Compose app in `software/android` that controls the GTFOg over Bluetooth LE and shows and records its accelerometer
stream. Package `io.github.jmacheta.gtfog`, minSdk 26, target/compile SDK 36, version 0.3.6 (code 9). Platform `BluetoothGatt` for the app's own protocol, Compose `Canvas` for charts, and the
Nordic nRF Connect Device Manager library (`no.nordicsemi.android:mcumgr-ble` 2.7.4) for firmware update only.

## Screens

Source files: `Queue.kt` (GATT operation queue and reconnect backoff, no Android types), `LinkPolicy.kt` (reconnect decisions and pending-write bookkeeping, no Android types), `Ble.kt` (GATT client), `Session.kt` (scan, recording), `Dfu.kt`, `Protocol.kt` (also the RF test table and payload `encodeRfTest`), `RecordService.kt`, `ScanScreen.kt`, `DeviceScreen.kt` (top bar, tabs, offline message), `ControlTab.kt`, `SettingsTab.kt`, `DebugTab.kt`,
`Chart.kt`, `Theme.kt` (theme, touch sizes, chips), `MainActivity.kt` (single activity, screen-on flag during an update, disconnect on destroy), `GtfogApp.kt` (Application, holds the `Session`).

| Screen | Contents |
| --- | --- |
| Scan | LE scan filtered by the service UUID; each device as "GTFOg · B3:0B" (last two address bytes), address, RSSI; tap to connect. First-run text (permission, then the system pairing dialog on the first connect). The scan runs only while the app is visible (it stops on Home and restarts on return) and stops after 30 s ("Scan again"); a failed scan shows its error. The last connected device is remembered (SharedPreferences) and connected to automatically when seen, once per app start. A permanently refused permission offers "Open app settings"; Bluetooth off offers "Enable Bluetooth"; on Android 11 and older a switched-off Location service gets a hint and "Open Location settings"; after a bad bond the error and "Open Bluetooth settings" (see BLE protocol) |
| Control (tab) | Battery icon drawn on a Canvas (red with "!" on fault, bolt while charging, grey when unknown; spoken as "Battery 82 %, charging"), fan readout with slider and -step/+step buttons (the device's fan step, "-5 %" / "+5 %"; TalkBack reads "Fan slower by 5 percent" / "Fan faster by 5 percent"), strobe switch, Mode: Manual / Motion. In Motion: "Slow down after" (15 s, 30 s, 1 min, 2 min, 4 min) and a chip row each for "Still", "Walking" and "Running" (Stop, 10 ... 100 %); with a firmware that has no per-activity speeds a note says to update. With the transport lock on, a banner says the fan and strobe stay off and how to unlock. A write that finally fails shows a snackbar "Could not change ..." and the control returns to the device's value In Motion the fan readout shows "Activity: Walking" below the number, and "Target N %" only when the speed differs from it. The slider and the -/+ buttons write the fan characteristic as before; the device applies them to the current activity |
| Settings (tab) | Stealth mode, Transport lock (unlock on the goggles: press + three times quickly), Auto shutdown (Off, 5, 10, 15, 30, 60 min), Fan step per click (1, 2, 5, 10, 20 %), Firmware card (installed version, update), model, manufacturer and the pairing note (new phones can pair only within 3 minutes after switch-on). Shows "Update the firmware to change settings" if the device lacks the settings characteristic; with old firmware (2-byte settings) the fan step and the motion mode are disabled with a note |
| Debug (tab) | Accelerometer, recording and the list of recordings below |
| Accelerometer (Debug) | Live switch (the whole row toggles; the Settings switches work the same way; disabled while recording, with the reason "Live stays on while recording. Stop the recording first."; if the accelerometer notifications cannot be enabled the switch goes back off with an error line, and a failed subscription of another characteristic shows an error line too), a note when the negotiated ATT MTU is below 247 (frames may be cut), scrolling x/y/z chart in g (x solid, y dashed, z dotted, besides red, green and blue), window 5/10/30 s, Y range auto/±2/±4/±8/±16 g, legend, \|a\|, measured samples/s, lost frames |
| RF carrier test (Debug) | Card shown only when the goggles have characteristic `a7f00008` (firmware 0.3.11+). Channel slider 0..39 with the MHz (default 19 = 2444 MHz; `rfTestMhz` in `Protocol.kt`, the same table as the firmware), duration chips 10, 30, 60, 120 s, Start with a confirmation ("The goggles will drop the connection and transmit a carrier on <MHz> for N s. Press any button on the goggles to stop earlier. They restart afterwards and the app reconnects."). Disabled while not connected and during a firmware update; Live and a recording are stopped first. After the write succeeds the card shows a countdown (`rfTestEndsMs`, whole seconds, then "Waiting for the goggles to restart…") while the client reconnects as usual: the goggles close a ready link (status 19), which is neither an error nor an early drop (those count only links that never became ready), so no stale-bond text appears; `rfTestEndsMs` is cleared when the services are ready again. A failed write shows "Could not start the RF test". The firmware behavior is in [firmware.md](firmware.md#rf-carrier-test) |
| Recording (Debug) | Record/Stop (a file that cannot be opened or written shows "Recording failed: ..."), file name, duration, sample count; list of recordings with Share and Delete (Delete asks to confirm) |

After connecting, the top bar keeps the device name, connection state and Disconnect (asks for confirmation while recording; disabled while a firmware update runs); system Back goes to the Control tab first and is ignored during an update; a bottom navigation bar switches the tabs.
Control and Settings show a message instead of empty controls while the link is down: "Connecting...", "Waiting for the goggles, reconnecting..." or,
when Bluetooth is off, "Bluetooth is off" with a Reconnect button. Debug (recordings) and a running firmware update stay visible.

The theme follows the system light/dark setting. Fan, strobe, mode and connect controls are at least 56 dp tall, all other touch
targets at least 48 dp.

The slider writes on release, the buttons on press. Fan, strobe, charger, battery and settings follow notifications. While a write
to fan, strobe or settings is queued or in flight, notifications and reads of that characteristic are ignored. Settings changes and
the fan buttons are computed from the latest client state, so quick taps do not use stale values. The chart redraws at about
30 fps from a 4096-sample ring buffer, only while Live or a recording is on and the app is visible; the numbers beside it update 4 times a second. The slider has the name "Fan speed" for TalkBack, the tab icons are decorative (the label is read), the scan cards are buttons, and the chart has a text summary.

## Permissions

| API level | Permissions |
| --- | --- |
| 31+ | `BLUETOOTH_SCAN` (`neverForLocation`), `BLUETOOTH_CONNECT` |
| below 31 | `BLUETOOTH`, `BLUETOOTH_ADMIN`, `ACCESS_FINE_LOCATION` |
| all | `FOREGROUND_SERVICE`, `FOREGROUND_SERVICE_CONNECTED_DEVICE`, `WAKE_LOCK`; `POST_NOTIFICATIONS` (API 33+, asked when recording starts) |

## BLE protocol as used

The device advertises the name `GTFOg` and service `a7f00001-5c1e-4b8e-9d2a-6f0e1c3b4d50`. Every characteristic needs an encrypted
link: the first access makes Android show the Just Works pairing dialog, and the bond is stored on the device. The app retries a
failed operation (ATT errors 5 and 15) every 1.5 s, up to 6 times (without limit while Android is creating the bond) while pairing completes.
If it still fails, or the disconnect status is 5, 15 or 137, or the bond is removed or refused (`ACTION_BOND_STATE_CHANGED` to none), or on Android 15+ the system reports a lost remote key (`ACTION_KEY_MISSING`, acted on at once as a stale bond), the app closes the link. If the phone had a bond (bond state BONDED before the failure) it shows "The goggles forgot this phone. Remove GTFOg in Android Bluetooth settings, then connect again" with a button for the Bluetooth settings
(a stale bond is expected after the device flash was erased). Without a bond (the pairing dialog was cancelled, or the goggles refused a new phone after their 3-minute pairing window) it shows "Pairing was refused. Turn the goggles off and on (pairing is open for 3 minutes after power-on), then connect again". Disconnect statuses 19 and 22 are ordinary disconnects; but if the phone is bonded and the link drops 3 times within 2 minutes right after connecting, before the services are found, with status 0, 19 or 22 (a peripheral that rejects encryption; range or radio statuses such as 8, 62 and 133 do not count), the stale-bond text is shown too. A first connect that fails with status 133, or whose service discovery fails with an error status, is retried up to 3 times with the backoff before giving up ("Could not connect, try again" / "Service discovery failed"); "Not a GTFOg" is shown only when discovery succeeded but the GTFOg service is missing. It requests ATT MTU 247 right after
connecting; the result is kept and shown in the Debug tab when it is lower. Connect, MTU and discovery must finish within 15 s (then the app closes the link and retries or reports "Could not
connect, try again"); a queued GATT operation with no callback within 5 s is dropped (not while a bond is being created); a late callback of a dropped operation is ignored (each operation has a key, the characteristic or descriptor, and a unique id; the first callback with the key of the last timed-out operation is taken as its late answer, so it cannot complete a newer operation with the same key; the late answer is awaited for one more 5 s only, so a genuine answer is not swallowed later; checked by `OpQueue.finish`). After 3 timeouts or refused calls in a row the GATT stack is taken as wedged: the client closes the link and reconnects with the backoff ("Connection stalled" if it never connected). After a lost link the client reconnects with `autoConnect` after 1, 2, 4 ... 30 s (back to 1 s after a successful connection). The client
registers for Bluetooth adapter changes: off closes the link ("Bluetooth is off"), on reconnects if the user still wants the
connection. Disconnect statuses are shown as text: 8 "Out of range", 19 "The goggles closed the connection", 0 and 22 "Connection
closed", 133 "Could not connect, try again".

| UUID | Name | Format | Access |
| --- | --- | --- | --- |
| `a7f00002-...` | Fan speed | u8 0..100 %; larger values give an ATT error | read, write, notify |
| `a7f00003-...` | Strobe | u8 0/1 | read, write, notify |
| `a7f00004-...` | Accelerometer | frame, below | notify; the stream runs only while notifications are on |
| `a7f00005-...` | Charger state | u8: 0 not charging (also charge done), 1 charging, 3 fault; 2 (completed) only from old firmware | read, notify |
| `a7f00006-...` | Settings | 8 bytes, below (6 or 2 on older firmware); written whole on every change | read, write, notify |
| `a7f00007-...` | Fan state | 2 bytes: speed in % (the big number on the Control tab) and activity level (0 still, 1 walking, 2 running, 255 manual); 1 byte on firmware before 0.3.2 | read, notify |
| `0x2A19` (`0x180F`) | Battery level | u8 % | read, notify |
| `0x2A24`, `0x2A29` (`0x180A`) | Model, manufacturer | string | read |

All UUIDs end in `-5c1e-4b8e-9d2a-6f0e1c3b4d50`.

Settings (v3): byte 0 flags (bit 0 stealth, bit 1 transport lock), 1 auto shutdown in minutes (0 = never), 2 fan step per
click in % (1..25, default 5), 3 fan mode (0 manual, 1 motion), 4 motion delay in s (5..255, default 60), 5, 6, 7 motion fan
in % while still, walking, running (0..100, defaults 20, 50, 100). Firmware 0.3.1 has 6 bytes (byte 5 is the still speed; the
per-activity rows are replaced by a note) and 0.2.0 has 2 bytes (extra settings shown as unsupported); writes keep the length
the device uses (8 for new firmware). Values read from the device are clamped to those ranges. The device changes settings too (the transport lock is cleared by pressing +
three times quickly) and notifies them; the app subscribes on connect.

Accelerometer frame (little endian): `u16 seq`, `u8 rate_hz` (100), `u8 n` (at most 40), then `n` times `int16 x, y, z` in milli-g
(range ±16 g). `seq` increases by 1 per frame and wraps; a gap counts as lost frames. A duplicate or reordered frame (seq not newer than the last accepted) is dropped: not plotted, not recorded, not counted (`SeqTracker`).
GATT operations go through a serial queue (Android allows one at a time). The queue (order, retry, timeout) and the backoff are the pure classes
`OpQueue` and `Backoff` in `Queue.kt`, and the decisions (early drops, first-connect and discovery retries, pending writes) are `LinkPolicy` and `PendingWrites` in `LinkPolicy.kt`, all unit-tested; `GtfogClient` only wraps `BluetoothGatt` around them. After every write (the last one of a burst), successful or not, the characteristic is read back (the fan also reads the fan state) so the screen shows the device's value; a failed one also shows "Could not change ...". Settings of 9 or more bytes and fan state of more than 2 bytes (future firmware) are read from the known bytes and the rest is ignored; settings are written back as 8 bytes. Flag bits above bit 1 read from the device are kept and written back unchanged. The chart time axis uses the `rate_hz` of the last frame. `disconnect()` calls `BluetoothGatt.disconnect()` and closes the gatt when Android reports the disconnect (after 1 s at the latest). Writes use the API 33 overloads (`writeCharacteristic(c, bytes, type)`, `writeDescriptor(d, value)`) and the older `value` path below API 33.

## Firmware update

Settings tab, Firmware card. The installed version is slot 0 of mcumgr image list, read when the tab is shown with a connection.
The Install button is disabled, with the reason "Charge to at least 50 % to update", when the battery is below 50 % or unknown (`dfuBlockReason` in `Protocol.kt`), and `Dfu.install()` checks it too. A file that is not newer than the installed one shows "Older than the installed version" or "Same version is already installed" (`versionHint` in `Dfu.kt`) and is not blocked: Install first asks "Install this version?" (`downgradeQuestion`), and "Install anyway" starts the update. "Update firmware..." opens the system file picker (`*/*`): a raw `zephyr.signed.bin` or the NCS `dfu_application.zip` (its single
`.bin` is used). The MCUboot header magic `0x96f3b83d` is checked and the file version (header offset 20) is shown; anything else
is an error; a file over 4 MB is "File too large" (the read itself stops at 4 MB, whatever size the provider reports). The file is read off the main thread, and the card shows why an equal or older version cannot be installed. "Install" runs `FirmwareUpgradeManager` in `TEST_AND_CONFIRM` mode over `McuMgrBleTransport` (a second GATT link to
the connected device; swap time 20 s, upload window 4, alignment 4): validate, upload (percent, Cancel while uploading), test, reset, confirm.
The device resets and `GtfogClient` reconnects; the file version is not trusted: after completion the app waits until the client
is connected again (at most 60 s), then reads slot 0 from the device and shows that (if the link does not come back the card says "Version unknown — reconnect to read it", and the read is retried on the next connection; no second GATT link is opened to goggles the user disconnected). A failed update shows "Update failed. The goggles keep running the firmware they started with; check the version after reconnecting." with the technical detail below it; after a failure in the reset or confirm step the app also reads the installed version once the goggles are back. When an update starts, Live and any recording are stopped (and cannot be started during it; the card says so). While it runs the screen stays on (a window flag set by `MainActivity` for the whole update, whichever tab is shown), Disconnect, Reconnect and Back are disabled, the top bar shows the phase and percent on every tab, and the card asks to keep the app open. The Control (fan and strobe are also disabled under the transport lock) and Settings controls and the Debug Live and Record controls are disabled too, each with a short reason line, so nothing else competes for the link. A watchdog fails the update (same failure text, detail "No answer from the goggles for 90 s"; the update is failed first, then cancelled) when mcumgr gives no state or progress callback for 90 s in any phase, so the controls are never locked for good; Cancel is shown only while uploading (the watchdog covers the other phases). There is no foreground service: if Android kills the app in the background, or the phone leaves range, the update is interrupted (the old firmware stays). The installed version is forgotten on every new connection and reconnect (so an outside change is noticed) and read when the Settings tab is shown and it is not known yet, so a failed read is also retried on each visit to the tab (and twice automatically, 3 s apart), one read at a time (Install refuses while a read runs), and the card says why it could not read it. A zip entry is read only up to 4 MB. The
library 2.7.4 is the newest that builds without raising compileSdk to 37 (2.8.0 and later require it). Helpers `parseImageVersion`
and `loadImage` are in `Dfu.kt`, tested in `DfuTest.kt` (the orchestration around `FirmwareUpgradeManager` is not unit tested: it needs the library and a device).


## Recording

Files go to `Android/data/io.github.jmacheta.gtfog/files/recordings/gtfog_YYYYMMDD_HHMMSS.csv` (`_1`, `_2` ... is appended if the name exists; the preamble `rate=` is the rate of the last frame, 100 Hz before the first). Record turns Live on if needed and
starts a foreground service (type `connectedDevice`) with an ongoing notification (chronometer, samples and lost frames refreshed every 5 s, Stop). If Android refuses to start the service (background start, API 31+: either the `startForegroundService` call or the service's `startForeground`) the recording stops with "Recording failed: ...". Stop and a lost link remove the notification at once; so does Live going off by itself (failed notification enable). The
service keeps the process, the GATT link and the file writer alive with the screen off. Each frame is flushed to the file. If the
link drops, the client reconnects with `autoConnect` and writes a `# reconnect at <ms>` line. Disconnect stops the recording, and so does any end of the link that the client closes itself (bad bond, not a GTFOg, failed first connect). Back on the Control tab finishes the activity; unless a recording runs (its service keeps the process and the link) or an update is running, `MainActivity.onDestroy` then disconnects the link, so the goggles (one connection) are free for another phone. The service holds a partial wake lock with a 4 h timeout. The list of recordings is refreshed after the writer is closed on that thread. The file is opened and written on a background thread; if that fails the recording stops with a message.

```
# start=2026-11-21T10:00:00+01:00, app=GTFOg 0.3.6, device=GTFOg by <manufacturer> <address>, rate=100 Hz
phone_time_ms,seq,index,x_mg,y_mg,z_mg
1763715600123,42,0,12,-998,40
1763715600123,42,1,10,-1001,38
# reconnect at 1763715700456
```

| Column | Meaning |
| --- | --- |
| `phone_time_ms` | Phone clock (Unix ms) when the frame was received; all samples of a frame share it |
| `seq` | Frame sequence number from the device |
| `index` | Sample position inside the frame |
| `x_mg`, `y_mg`, `z_mg` | Milli-g |

Frames arrive in jittery bursts, so order samples by `seq` and `index`; at 100 Hz the sample spacing is 10 ms.

If the process is killed during a recording, the file stays valid up to the last flushed frame; the recording is not resumed.

## Build and install

JDK 17 or newer that AGP 8.9 accepts (Android Studio's bundled JDK 21 works; JDK 25 does not). Android SDK platform 36;
put its path in `software/android/local.properties` as `sdk.dir=...` (git-ignored).

```
cd software/android
./gradlew assembleDebug assembleRelease testDebugUnitTest lintDebug
./gradlew installDebug          # phone with USB debugging
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb pull /sdcard/Android/data/io.github.jmacheta.gtfog/files/recordings/ .
```

Release builds use R8 (`isMinifyEnabled`, `isShrinkResources`) with `app/proguard-rules.pro`, which keeps `io.runtime.mcumgr.**` and the
Jackson annotations (mcumgr maps CBOR onto its model classes by reflection). The release is signed only if the Gradle properties
`gtfog.storeFile`, `gtfog.storePassword`, `gtfog.keyAlias` and `gtfog.keyPassword` are set (for example in `~/.gradle/gradle.properties`);
otherwise `app-release-unsigned.apk` is produced. No keys are in the repository. Lint (`lint {}` in `app/build.gradle.kts`) aborts on errors; warnings are not errors (version and wakelock warnings remain); the Gradle wrapper is pinned with `distributionSha256Sum`. There is no Gradle dependency verification metadata (heavy for this project; the wrapper checksum is pinned). The launcher icon is adaptive (`mipmap-anydpi-v26`,
vector fan in a ring, with a monochrome layer).

The testable core is `Protocol.kt` (frame decode, sequence gaps, ring buffer, CSV lines, charger mapping, settings v3 and fan state, labels and error texts), covered by JVM unit tests
in `app/src/test`. CI job `android` in `.github/workflows/build.yml` runs the same Gradle command (including `assembleRelease`) and uploads the debug APK as
`gtfog-android-debug`.
