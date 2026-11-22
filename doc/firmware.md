# Firmware

Zephyr C++ application in `software/firmware`, built with MCUboot through sysbuild. Design rules are in
[design/principles.md](design/principles.md); build steps in [design/build_and_tooling.md](design/build_and_tooling.md).
Everything below describes the code as it is; unclear points are in [Open questions](#open-questions).

## Modules

Pure logic, no Zephyr includes (`src/logic/`, tested on the host):

| File | Contents |
| --- | --- |
| `buttons.hpp/.cpp` | `button_gestures`: turns sampled button levels and time into `button_event`s (including the unlock gesture); `button_timing` holds the thresholds |
| `battery.hpp/.cpp` | `decode_charger_state`, `battery_percentage` (Li-ion curve), `battery_guard` (low-battery cutoff) |
| `accel.hpp/.cpp` | `pack_accel`: accelerometer stream packet; `counts_to_milli_g`, `accel_samples_per_packet`; `activity_classifier` |
| `controller.hpp/.cpp` | `controller`: fan speed, strobe, signals, power off; abstract `outputs`; `fan_duty_permille`, `pwm_pulse_ns` |

Board layer (`src/`):

| File | Contents |
| --- | --- |
| `app.hpp` | `with_controller(f)`: runs `f(controller &)` under a mutex; declarations shared by the board files |
| `board.cpp` | `board_outputs` (implements `outputs`): PWM fan, PWM strobe and RGB LED patterns (`k_work`), System OFF; owns the `controller` |
| `buttons.cpp` | Button GPIO interrupts, `buttons` thread feeding `button_gestures`, wake-up arming |
| `battery.cpp` | `battery` thread: samples VBAT and the charger STAT pins every 10 s, counts the auto-off time (also when the ADC fails) and feeds the watchdog |
| `ble.cpp` | Advertising, GATT control service, Battery Service, Device Information Service (the mcumgr SMP service comes from NCS) |
| `accelerometer.cpp` | `accelerometer` thread: powers the ADXL345 only while a phone streams or motion mode is on; feeds the activity classifier and the stream |
| `main.cpp` | `main()`: `power_on`, `ble_init`, confirms the running image once Bluetooth is up; `SYS_INIT` hook that sets the GPIO supply (UICR `REGOUT0`) to 3.3 V and resets once if it is not |

## Threads

| Thread | Priority | Stack | Job |
| --- | --- | --- | --- |
| `main` | Zephyr default | 2048 B (`CONFIG_MAIN_STACK_SIZE`) | Powers the controller on, starts BLE, confirms the image, returns |
| `buttons` | 5 | 1024 B | Waits for a pin change; polls every 20 ms while a button is down |
| `battery` | 7 | 1024 B | Starts after 500 ms; samples every 10 s, then feeds the watchdog |
| `accelerometer` | 10 | 1536 B | Starts after 100 ms; sleeps with the sensor unpowered; while streaming or in motion mode powers it and reads the FIFO every 100 ms |

The strobe, the LED patterns, BLE notifications, saving the settings and the advertising restart run on the system workqueue
(2048 B stack). Every thread reaches the controller through `with_controller`.

## Controller

`controller` (`src/logic/controller.cpp`) holds the target fan speed (0..100 %, what the user set and BLE reads), the strobe state, the battery percentage and
the `device_settings`, and drives an `outputs` object. Signals go to `outputs::show()` only when stealth mode is off.

| Input | Effect |
| --- | --- |
| `power_on()` | Signal `power_on`, strobe off, fan 0 (in motion mode the still speed, level still) |
| Any button while `transport_lock` is on | Signal `locked`, nothing else; only `power_off` and `unlock` still work |
| Button `plus` / `minus` | Fan speed `+steps * fan_step` / `-steps * fan_step`, clamped to 0..100, on the target of the current activity level in motion mode; signal `speed_up`, `speed_down`, or `speed_limit` when the result reaches 0 or 100 |
| Button `unlock` | While locked: clears `transport_lock`, stores the settings, signal `unlocked`. Otherwise the same as `plus` with 1 step |
| Button `toggle_strobe` | Strobe on or off |
| Button `power_off` | Strobe off, fan 0, signal `power_off`, `outputs::power_off()` |
| `set_fan_speed(percent)` (BLE) | Fan speed, clamped to 100; no signal. In motion mode it sets the target of the current activity level and stores the settings |
| `set_strobe(on)` (BLE) | Strobe on or off; no signal |
| `battery_status` | Stores the percentage; when `battery_guard` fires: strobe off, fan 0, signal `low_battery`, power off. Otherwise signal `battery_warning` once when the level reaches 10 % or less, armed again above 15 % |
| `elapsed(seconds, phone_connected)` (every 10 s) | Counts idle time: fan 0, strobe off and no phone; a button press restarts it. After `auto_off_minutes` (0 never): the same as `power_off` |
| `observe(level)` (accelerometer, once per second, motion mode) | Raw activity level of the last second; may switch the level, see [Motion mode](#motion-mode) |
| `update_settings(settings)` (BLE) | Stores the settings, calls `outputs::settings_changed` (saved to flash and notified by the board), applies the fan mode |
| `configure(settings)` (flash, at boot) | Stores the settings only |

### Settings

`device_settings`, written over BLE and kept in flash under the settings key `gtfog/cfg` (loaded before `power_on()`, so
stealth mode also hides the power-on signal). Eight bytes (`encode`, `decode_settings`); the 6-byte format of firmware 0.3.1 (byte 5 is the still speed, the other
speeds at their defaults) and the 2-byte format of firmware 0.2.0 (bytes 0 and 1, the rest at their defaults) are still
accepted on writes and in flash, anything else is rejected:

| Byte | Contents |
| --- | --- |
| 0 | Flags: bit 0 stealth (the status LED never lights up), bit 1 transport lock (buttons only wake, power off and unlock); other bits must be 0 |
| 1 | Auto-off in minutes, 0 = never |
| 2 | Fan step per click in %, 1..25, default 5 |
| 3 | Fan mode: 0 manual, 1 motion |
| 4 | Motion mode: "slow down after", seconds of lower activity before the level drops, 5..255, default 60 |
| 5 | Motion mode: fan speed in % while still, 0..100, default 20 |
| 6 | Motion mode: fan speed in % while walking, 0..100, default 50 |
| 7 | Motion mode: fan speed in % while running, 0..100, default 100 |

A write from Bluetooth (or the unlock gesture) goes through `controller::update_settings`; `board_outputs` hands the
copy to `ble_settings_changed`, which saves it in flash and notifies the characteristic from the system workqueue, so the
Bluetooth thread never waits for flash. Values loaded at boot are not saved again.

### Motion mode

With fan mode 1 the accelerometer is powered and read every 100 ms. `activity_classifier` takes the standard deviation of
the acceleration magnitude over each second (100 samples) and reports a raw level: below 80 mg still, below 300 mg walking,
else running (`walk_threshold_milli_g`, `run_threshold_milli_g`; guesses, to calibrate with Debug-tab recordings). The
controller (`observe`) keeps the current level, starting at still: it goes up when the raw level has been above it for 2
consecutive seconds (to the lowest level seen in those seconds), and down to the raw level when that has stayed below the
current level for `motion_delay_s` seconds; a second at the current level restarts the countdown. The fan runs at the target
of the current level (`motion_speed[level]`, bytes 5..7). The buttons and Bluetooth fan writes change the target of the
current level and store the settings, so `fan_speed()` and the fan characteristic are that target. Manual mode keeps its own
target and ignores the activity; switching mode resets the level to still.

## Buttons

Two active-low buttons with pull-ups: minus and plus. A GPIO edge wakes the `buttons` thread; while a button is down it
re-evaluates every 20 ms. Thresholds are `button_timing` defaults in `buttons.hpp`.

| Gesture | Result |
| --- | --- |
| Short press: released after 50 ms up to under 500 ms | Plus: fan +1; minus: fan -1 (reported on release) |
| Shorter than 50 ms | Ignored (bounce) |
| Hold of at least 500 ms | Repeats every 200 ms; steps per repeat = `1 + hold_time / 3 s`, at most 5 |
| Held for at least 30 s | Stuck: ignored until released |
| Both held, released after 2 s and before 5 s | Strobe toggles, once, on release |
| Both held for 5 s | Power off, while still held (the strobe does not toggle) |
| Three short presses of plus, the third within 2 s of the first | Third press is `unlock` instead of plus (see [Controller](#controller)) |

Once both buttons are down together, single-button actions stay off until both are released; letting one go before 2 s cancels the chord; letting one go later toggles the strobe. One action is returned per sample.

## Fan

PWM0 channel 3, 5 ms period, normal polarity. `fan_duty_permille(percent, min_duty)`: 0 % is off; 1..100 % maps
linearly onto `fan_min_duty_permille` (20 %) .. 100 % duty, so a running fan never stalls. `pwm_pulse_ns` computes the
pulse width in 64 bits (5 ms x 1000 overflows 32 bits). In motion mode the fan speed follows the activity, see [Motion mode](#motion-mode).

## Strobe

PWM1 channel 0, 1 ms period, normal polarity. On: full duty for 100 ms, off for 100 ms, repeated by a `k_work`. Off:
the work is cancelled and the duty is 0.

## Status LED

RGB LED on PWM0 channels 0..2 (inverted polarity, 5 ms period: the channels of one PWM instance share a period). `pattern_of` in `board.cpp` maps a signal to a color
and blinks; a new signal replaces the running one.

| Signal | Color | Blinks | On / off |
| --- | --- | --- | --- |
| `power_on` | Green | 3 | 50 ms / 50 ms |
| `speed_up` | Green | 1 | 25 ms |
| `speed_down` | Yellow | 1 | 25 ms |
| `speed_limit` | Magenta | 1 | 50 ms |
| `low_battery` | Red | 3 | 200 ms / 200 ms |
| `locked` | Red | 1 | 50 ms |
| `unlocked` | Green | 2 | 100 ms / 100 ms |
| `battery_warning` | Red | 2 | 100 ms / 100 ms |
| `power_off` | Cyan | 1 | 300 ms |

## Battery and charger

- `battery` thread: ADC channel `vbat` (AIN4, P0.28, gain 1/6, 12 bit) times `vbat_divider` (2, a 47k/47k divider on
  the POWER sheet), and the charger STAT1 (P0.03) and STAT2 (P0.02) levels, every 10 s.
- Charger BQ25185 (POWER sheet). `decode_charger_state(stat1, stat2)` per the datasheet status table: both high is not
  charging (charge done, sleep or no input power), STAT1 high and STAT2 low is charging, STAT1 low is a fault (with STAT2
  high recoverable: input overvoltage, TS hot or cold, thermal shutdown, system short; both low latched). Board 2 with a
  battery on USB reads STAT1 low, STAT2 high, a recoverable fault: check the TS network and the STAT wiring.
- `battery_percentage(mV)`: linear interpolation of a Li-ion curve: 3300 mV 0 %, 3500 5 %, 3600 10 %, 3700 30 %,
  3800 50 %, 3900 65 %, 4000 80 %, 4200 100 %. Reported through the BLE Battery Service. The charger state also goes to the charger characteristic.
- `battery_guard`: 3 consecutive samples below 3300 mV while the charger is not charging make the
  controller show the red low-battery signal and power off. Any other sample resets the count.
- The `vdd` ADC channel (VDDH / 5) is in the devicetree and not sampled.

## Bluetooth LE

Peripheral, device name `GTFOg`, one connection at a time (`CONFIG_BT_MAX_CONN=1`), ATT MTU 247, data length 251.
GATT caching is on, so bonded phones get Service Changed and rediscover the database after a firmware update.
Advertising data: flags and the control service UUID; the name is in the scan
response. Advertising restarts after a disconnect.

| Service | UUID | Characteristic | Properties |
| --- | --- | --- | --- |
| GTFOg control | `a7f00001-5c1e-4b8e-9d2a-6f0e1c3b4d50` | Fan speed `a7f00002-...`: uint8, 0..100 | read, write, notify |
| | | Strobe `a7f00003-...`: uint8, 0 or 1 | read, write, notify |
| | | Accelerometer `a7f00004-...`: stream, see below | notify |
| | | Charger `a7f00005-...`: uint8, 0 not charging, 1 charging, 3 fault (2 is no longer sent) | read, notify |
| | | Settings `a7f00006-...`: 6 bytes, see [Settings](#settings) | read, write, notify |
| | | Fan state `a7f00007-...`: 2 bytes, [0] speed the fan runs at in %, [1] activity level 0 still, 1 walking, 2 running, 255 manual mode; notified when either changes | read, notify |
| mcumgr SMP | standard SMP UUID | Firmware update, see [Firmware update](#firmware-update-and-mcuboot) | write, notify |
| Battery Service | standard | Battery level, updated every 10 s | per Zephyr BAS |
| Device Information Service | standard | Model number `GTFOg`, manufacturer `elmaczete_customs` | per Zephyr DIS |

The control, charger and SMP characteristics and all CCC descriptors need an encrypted link (Just Works pairing); bonds are stored
in settings (NVS, `storage_partition`). Writes must have the exact length (1 byte, settings 2, 6 or 8); out-of-range values are rejected. Changes made by the
buttons are notified to a connected phone. The settings store holds the device settings and the bonds: up to 4
(`CONFIG_BT_MAX_PAIRED`), and pairing a fifth device replaces the oldest.

New pairings are accepted only in the first 3 minutes after boot (`pairing_accept` in `ble.cpp`; every wake from System OFF is a boot); later requests are refused with `BT_SECURITY_ERR_PAIR_NOT_ALLOWED`. Bonded phones reconnect at any time. Pairing stays Just Works.

## Accelerometer

The ADXL345 is accessed over raw I2C (no Zephyr sensor driver, `CONFIG_SENSOR` off). The `accelerometer` thread powers it
only while a phone subscribes to characteristic `a7f00004` or motion mode is on; otherwise the power pin (P1.09) is low.
Each power-up drives the pin low for 100 ms, then high, waits 500 ms, checks the device ID (0xE5) on I2C0 (address 0x53)
and sets 100 Hz, +-16 g full resolution and FIFO stream mode (a failure is retried after 10 s). While active it reads
the FIFO every 100 ms (the 32-sample FIFO holds 320 ms), feeds `activity_classifier` and, in motion mode, `controller::observe` once per second,
and sends notifications while subscribed; the FIFO is flushed on each new subscription. The power is cut when
neither is needed and by `accelerometer_power_off()` before System OFF. INT1 (P0.05) and INT2 (P0.04) are in the
devicetree and unused.

Notification frame (little endian, `pack_accel`): u16 `seq` (starts at 0 per subscription), u8 `rate_hz` (100), u8 `n`,
then `n` x int16 x, y, z in mg (counts x 3.9). `n` is up to 40 at MTU 247 (`accel_samples_per_packet`). A full
controller buffer drops a packet; a gap in `seq` shows it. Checked ad hoc on a board: about 100 Hz, no gaps; this is not
in `ble_test.py`.

## Power off and wake-up

`board_outputs::power_off()` runs under the controller mutex, so it waits for no work item: it cancels the LED pattern
and plays the rest of it itself with `k_msleep` (the system workqueue may be blocked on the mutex), turns the LED off,
cuts the accelerometer power, waits until both buttons are released (at most 10 s), arms both as level-active wake
sources and calls `sys_poweroff()` (System OFF). A press wakes the chip through a reset.

## Watchdog

The nRF hardware watchdog (`wdt0`, Zephyr `wdt` API, `CONFIG_WATCHDOG`) has a 30 s timeout, reset SoC, and pauses while
the debugger halts the CPU. The `battery` thread starts it and feeds it every 10 s, right after it took and released the
controller mutex, so a stuck mutex or thread resets the device (the fan is off after a reset). Not verified on hardware.

## Firmware update and MCUboot

USB in the application is gone; on the board USB is for charging, except for the MCUboot recovery below.

- Flash map (`gtfog.dts`): see [hardware.md](hardware.md#flash-map). MCUboot mode is sysbuild
  `SB_CONFIG_MCUBOOT_MODE_SWAP_USING_MOVE` (swap with revert, no scratch partition) with downgrade prevention.
- Update over Bluetooth: mcumgr SMP (`CONFIG_NCS_SAMPLE_MCUMGR_BT_OTA_DFU`) into slot 1. The link must be encrypted
  (`CONFIG_MCUMGR_TRANSPORT_BT_PERM_RW_ENCRYPT`), so pair first.
- Update with `smpmgr` (`pip install smpmgr`), verified on hardware; a 210 KB image uploads in about 16 s:

```sh
smpmgr --ble <address> image upload build/firmware/zephyr/zephyr.signed.bin
smpmgr --ble <address> image state-write <hash of slot 1>   # marks the image for test
smpmgr --ble <address> os reset
```

  `smpmgr upgrade` uploads and resets but does not mark the image, so it does not install it.
- Revert: after the swap the new image runs unconfirmed. `main()` confirms it (`boot_write_img_confirmed`) only after
  Bluetooth initialized; an unconfirmed image is reverted by MCUboot on the next reset (verified on hardware both ways).
- The image version comes from `software/firmware/VERSION` (0.2.0). Downgrade prevention rejects an older or equal
  image, so raise it for every update.
- Recovery: MCUboot has serial recovery over USB CDC ACM (mcumgr protocol, VID:PID `1209:EACD`, manufacturer
  `elmaczete_customs`, product `GTFOg bootloader`). Hold the plus button for more than 1 s (`CONFIG_BOOT_SERIAL_DETECT_DELAY=1000`, so a normal press that wakes the device does not enter recovery) while the device wakes or is plugged in (devicetree
  alias `mcuboot-button0` = `button_plus` in `sysbuild/mcuboot.overlay`), then
  `smpmgr --port /dev/ttyACMx image upload ...`. MCUboot builds this on the legacy USB device stack (as it does for
  CDC ACM recovery on nRF52); only the bootloader has USB. Not tested on hardware yet.
- MCUboot status hooks (`software/firmware/mcuboot_hooks/`, linked into the MCUboot image by `sysbuild.cmake`): at
  startup MCUboot turns the fan, the RGB LED and the strobe off; while it installs an update the red strobe blinks at
  5 Hz (hardware PWM, 200 ms period) and stops before the application starts.
- Boards with an MCUboot built for the old flash map need the new MCUboot over SWD once.
- Images are signed with the public MCUboot development key (`root-rsa-2048`): anyone can sign an image the bootloader
  accepts; see [Release signing key](design/build_and_tooling.md#release-signing-key).

## Logging

Off in the application (`CONFIG_LOG=n`, no boot banner); `LOG_*` calls stay in the code. To debug, set `CONFIG_LOG` and
`CONFIG_LOG_BACKEND_RTT` (plus `CONFIG_USE_SEGGER_RTT`) and read it with a J-Link RTT client or `nrfutil`. MCUboot has
logging off.

## Open questions

- The activity thresholds (80 and 300 mg) and the speed defaults are not calibrated; the watchdog, the 1 s recovery hold and the pairing window are not tested on hardware.
- The VBAT divider needs checking on a board, and board 2 reports a charger fault on USB (see [Battery and charger](#battery-and-charger)).
- MCUboot USB serial recovery is not tested on hardware.
- BLE works on a second board (`ble_test.py` passes); the first board transmits (RADIO in TX, checked over SWD) but no
  host receives it, so its RF path or 32 MHz crystal is suspect.
- The MCUboot signing key is the public development key; a private key is needed before devices are given out.
