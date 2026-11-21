# Android app

Kotlin / Jetpack Compose app in `software/android` that controls the GTFOg over Bluetooth LE and shows and records its accelerometer
stream. Package `io.github.jmacheta.gtfog`, minSdk 26, target/compile SDK 35. No BLE or chart library: platform `BluetoothGatt` and
Compose `Canvas`.

## Screens

| Screen | Contents |
| --- | --- |
| Scan | LE scan filtered by the service UUID; name, address, RSSI; tap to connect. Asks for permissions and offers to enable Bluetooth |
| Control (tab) | Battery icon drawn on a Canvas, filled to the level with the % beside it (bolt while charging; red with "!" and "Charger fault" on fault; grey outline when unknown), fan readout with slider and -5/+5, strobe switch, Mode (Manual; Motion disabled, coming soon) |
| Settings (tab) | Stealth mode, Transport lock, Auto shutdown (Off, 5, 10, 15, 30, 60 min), model and manufacturer. Shows "Update the firmware to change settings" if the device lacks the settings characteristic |
| Debug (tab) | Accelerometer, recording and the list of recordings below |
| Accelerometer (Debug) | Live switch, scrolling x/y/z chart in g, window 5/10/30 s, Y range auto/±2/±4/±8/±16 g, legend, \|a\|, measured samples/s, lost frames |
| Recording (Debug) | Record/Stop, file name, duration, sample count; list of recordings with Share and Delete |

After connecting, the top bar keeps the device name, connection state and Disconnect; a bottom navigation bar switches the tabs. The app is dark-only.

The slider writes on release, the buttons on press. Fan, strobe, charger and battery follow notifications. While a write to fan, strobe or settings is queued or in flight, notifications and reads of that characteristic are ignored, so quick taps on -5/+5 do not use stale values. The chart redraws at about
30 fps from a 4096-sample ring buffer.

## Permissions

| API level | Permissions |
| --- | --- |
| 31+ | `BLUETOOTH_SCAN` (`neverForLocation`), `BLUETOOTH_CONNECT` |
| below 31 | `BLUETOOTH`, `BLUETOOTH_ADMIN`, `ACCESS_FINE_LOCATION` |
| all | `FOREGROUND_SERVICE`, `FOREGROUND_SERVICE_CONNECTED_DEVICE`, `WAKE_LOCK`; `POST_NOTIFICATIONS` (API 33+, asked when recording starts) |

## BLE protocol as used

The device advertises the name `GTFOg` and service `a7f00001-5c1e-4b8e-9d2a-6f0e1c3b4d50`. Every characteristic needs an encrypted
link: the first access makes Android show the Just Works pairing dialog, and the bond is stored on the device. The app retries a
failed operation (ATT errors 5 and 15) every 1.5 s, up to 6 times, while pairing completes. It requests ATT MTU 247 right after
connecting.

| UUID | Name | Format | Access |
| --- | --- | --- | --- |
| `a7f00002-...` | Fan speed | u8 0..100 %; larger values give an ATT error | read, write, notify |
| `a7f00003-...` | Strobe | u8 0/1 | read, write, notify |
| `a7f00004-...` | Accelerometer | frame, below | notify; the stream runs only while notifications are on |
| `a7f00005-...` | Charger state | u8: 0 not charging (also charge done), 1 charging, 3 fault; 2 (completed) only from old firmware | read, notify |
| `a7f00006-...` | Settings | 2 bytes: flags (bit 0 stealth, bit 1 transport lock), auto shutdown in minutes (0 = never); written whole on every change | read, write |
| `0x2A19` (`0x180F`) | Battery level | u8 % | read, notify |
| `0x2A24`, `0x2A29` (`0x180A`) | Model, manufacturer | string | read |

All UUIDs end in `-5c1e-4b8e-9d2a-6f0e1c3b4d50`.

Accelerometer frame (little endian): `u16 seq`, `u8 rate_hz` (100), `u8 n` (at most 40), then `n` times `int16 x, y, z` in milli-g
(range ±16 g). `seq` increases by 1 per frame and wraps; a gap counts as lost frames. Duplicate or reordered frames are not counted.
GATT operations go through a serial queue (Android allows one at a time).

## Recording

Files go to `Android/data/io.github.jmacheta.gtfog/files/recordings/gtfog_YYYYMMDD_HHMMSS.csv`. Record turns Live on if needed and
starts a foreground service (type `connectedDevice`) with an ongoing notification (duration, samples, lost frames, Stop). The
service keeps the process, the GATT link and the file writer alive with the screen off. Each frame is flushed to the file. If the
link drops, the client reconnects with `autoConnect` and writes a `# reconnect at <ms>` line. Disconnect stops the recording.

```
# start=2026-11-21T10:00:00+01:00, app=GTFOg 0.1.0, device=GTFOg by <manufacturer> <address>, rate=100 Hz
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

## Build and install

JDK 17 or newer that AGP 8.9 accepts (Android Studio's bundled JDK 21 works; JDK 25 does not). Android SDK 35 with build-tools 35.0.0;
put its path in `software/android/local.properties` as `sdk.dir=...` (git-ignored).

```
cd software/android
./gradlew assembleDebug testDebugUnitTest lintDebug
./gradlew installDebug          # phone with USB debugging
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb pull /sdcard/Android/data/io.github.jmacheta.gtfog/files/recordings/ .
```

The testable core is `Protocol.kt` (frame decode, sequence gaps, ring buffer, CSV lines, charger mapping), covered by JVM unit tests
in `app/src/test`. CI job `android` in `.github/workflows/build.yml` runs the same Gradle command and uploads the APK as
`gtfog-android-debug`.

## Open items

- TODO: firmware update over SMP (mcumgr service `8D53DC1D-1DB7-4CD3-868B-8A527460AA84`). Not implemented.
- If the process is killed during a recording, the file stays valid up to the last flushed frame but the recording is not resumed.
- Not tested on a phone yet; only build, unit tests and lint ran.
