# Firmware

Zephyr C++ application in `software/firmware`, built with MCUboot through sysbuild. Design rules are in
[design/principles.md](design/principles.md); build steps in [design/build_and_tooling.md](design/build_and_tooling.md).

## Modules

Pure logic, no Zephyr includes (headers in `src/include/`, sources in `src/`, tested on the host; Boost.SML is fetched at configure time):

| File | Contents |
| --- | --- |
| `gestures.hpp/.cpp` | `button_gestures`: turns sampled button levels and time into `button_event`s (including the unlock gesture); `button_timing` holds the thresholds |
| `battery_model.hpp/.cpp` | `decode_charger_state`, `battery_percentage` (Li-ion curve), `battery_guard` (low-battery cutoff) |
| `rf_test.hpp` | `rf_test_frequency_mhz` (BLE channel index to MHz), `decode_rf_test` (payload of the RF test characteristic) |
| `motion.hpp/.cpp` | `pack_accelerometer_samples`: accelerometer stream packet; `counts_to_milli_g`, `accelerometer_samples_per_packet`; `activity_classifier`; `fifo_watchdog` |
| `controller.hpp/.cpp` | `controller_table`: the Boost.SML transition table; `controller_data` (the steps it calls); `controller`, the wrapper that turns calls into events; abstract `outputs`; settings encoding; `fan_duty_permille`, `pwm_pulse_ns` |

Board layer (`src/`):

| File | Contents |
| --- | --- |
| `include/app.hpp` | `with_controller(f)`: runs `f(controller &)` under a mutex; declarations shared by the board files |
| `board.cpp` | `board_outputs` (implements `outputs`): PWM fan, PWM strobe and RGB LED patterns (`k_work`), System OFF; owns the `controller` |
| `buttons.cpp` | Button GPIO interrupts, `buttons` thread feeding `button_gestures`, wake-up arming |
| `battery.cpp` | `battery` thread: samples VBAT and the charger STAT pins every 10 s, counts the auto-off time from `k_uptime_get` (also when the ADC fails) and feeds the watchdog (right after the controller mutex section, before the BLE notifications); `battery_sample()` for the boot check |
| `ble.cpp` | Advertising, GATT control service, Battery Service, Device Information Service (the mcumgr SMP service comes from NCS) |
| `rf_test.cpp` | `rf_test` thread: the RF carrier test (see [RF carrier test](#rf-carrier-test)) |
| `accelerometer.cpp` | `accelerometer` thread: powers the ADXL345 only while a phone streams or motion mode is on (the thread owns the power pin); feeds the activity classifier and the stream; power-cycles a silent sensor |
| `main.cpp` | `main()`: loads the settings (`app_settings_load`), `ble_init`, checks the battery (`check_start`), `power_on`, then confirms the running image once Bluetooth is up; `SYS_INIT` hook that sets the GPIO supply (UICR `REGOUT0`) to 3.3 V and resets once, only when the field still has the erased value 7 (any other value is left alone, to avoid a reset loop) |

## Threads

| Thread | Priority | Stack | Job |
| --- | --- | --- | --- |
| `main` | Zephyr default | 2048 B (`CONFIG_MAIN_STACK_SIZE`) | Starts BLE, checks the battery, powers the controller on, confirms the image, returns |
| `buttons` | 5 | 2048 B | Waits for a pin change; polls every 20 ms while a button is down, or always when the interrupt setup failed (`fault` once) |
| `battery` | 7 | 2048 B | Starts after 500 ms; samples every 10 s, then feeds the watchdog |
| `rf_test` | 6 | 2048 B | Sleeps until a phone writes the RF test characteristic, then runs the carrier test and reboots |
| `accelerometer` | 10 | 1536 B | Starts after 100 ms; sleeps with the sensor unpowered; while streaming or in motion mode powers it and reads the FIFO every 200 ms |

The strobe, the LED patterns, BLE notifications, saving the settings and the advertising restart run on the system workqueue
(2048 B stack). Every thread reaches the controller through `with_controller`. The `buttons` and `battery` stacks are 2048 B because power-off (flash save) runs in them.

## Controller

`controller` (`src/include/controller.hpp`, `src/controller.cpp`) is a Boost.SML state machine: every input below is an event of `controller_table` (`events::button`, `events::battery_sample`, `events::elapsed` and so on), processed under the controller mutex. The table has five orthogonal regions that each see every event, in this order: power (`running`, `powered_off`; the first matching row wins), transport lock (`unlocked` / `locked`; the button rows that depend on the lock), fan mode (`manual` / `motion`; the activity tracking row, which also needs the lock off) and the settings write (`synced`; stores, applies and publishes a user-written settings value after the other regions have changed state). The lock and the mode are only these states: `settings()` fills the two flags of `device_settings` from them, and the loaded (flash) and Bluetooth-written settings are `settings_received` events that move the states (`unlocked + settings_received [sets_lock] = locked` and so on); the unlock gesture is `locked + button [pressed_unlock] = unlocked`. What happens on a change (activity level and counters restart, as the accelerometer is off under the lock; for the gesture also fan and strobe restored, settings saved, the unlocked signal) is in `on_entry` actions of the states, so it sees the new state. A flash load at boot changes the states but shows no signal and saves nothing. After `powered_off` the power region ignores everything; on the board `outputs::power_off()` does not return anyway. It holds the target fan speed (0..100 %, what the user set; BLE reads the effective speed, `fan_speed()`), the strobe state, the battery percentage and
the `device_settings`, and drives an `outputs` object. Signals go to `outputs::show()` only when stealth mode is off. Stealth is strict: it also hides `low_battery`, `battery_warning` and `fault`, so the fan simply stops at the battery cutoff; the phone shows the battery level, the charger state and the fan state.

| Input | Effect |
| --- | --- |
| `check_start(status)` (`main()`, one ADC sample before `power_on`) | Below 3400 mV and not charging: signal `low_battery` and power off at once, the fan never starts (`battery_allows_start`). No ADC: the device starts |
| `power_on()` | Signal `power_on`, strobe off, fan 0 (in motion mode the still speed, level still; under the transport lock always 0) |
| Any button while `transport_lock` is on | Signal `locked` once per press (not on the repeats of a held button), nothing else; only `power_off` and `unlock` still work |
| `transport_lock` on | `fan_speed()` is 0 and `strobe()` false in both modes, also for Bluetooth writes and `power_on`; turning the lock on (Bluetooth) stops a running fan and strobe. The targets are kept, so unlocking restores them |
| Button `plus` / `minus` | Fan speed `+steps * fan_step` / `-steps * fan_step`, clamped to 0..100, on the target of the current activity level in motion mode; signal `speed_up`, `speed_down`, or `speed_limit` when the result reaches 0 or 100 |
| Button `unlock` | While locked: clears `transport_lock`, stores the settings, signal `unlocked`. Otherwise the same as `plus` with 1 step |
| Button `toggle_strobe` | Strobe on or off |
| Button `power_off` | Strobe off, fan 0, signal `power_off`, `outputs::power_off()` |
| `set_fan_speed(percent)` (BLE) | Fan speed, clamped to 100; no signal. In motion mode it sets the target of the current activity level and stores the settings |
| `set_strobe(on)` (BLE) | Strobe on or off; no signal |
| `battery_status` | Stores the percentage; when `battery_guard` fires: strobe off, fan 0, signal `low_battery`, power off. Otherwise signal `battery_warning` once after 3 samples in a row (30 s) at 10 % or less, armed again above 15 %; the curve is for a rested cell, so the fan load is a calibration item |
| `elapsed(seconds, phone_connected)` (every 10 s, real elapsed time) | Counts idle time: fan 0 (in motion mode: level still, whatever the still speed), strobe off and no phone; a button press restarts it. Only an encrypted connection (security level 2 or higher, set by the `security_changed` callback and cleared on disconnect) counts as a phone; an unpaired link does not. After `auto_off_minutes` (0 never): the same as `power_off`. Motion mode at level still counts as idle on purpose (goggles left on a shelf turn off), so a user who stays motionless for `auto_off_minutes` is powered off although the fan runs at the still speed; set auto-off to 0 or move, an encrypted phone connection also keeps it on. While `transport_lock` is on the limit is 1 minute whatever `auto_off_minutes` says (also 0 never), so a locked device woken in a bag turns itself off; an encrypted phone connection or a button press restarts the count |
| `battery_unreadable()` (`battery` thread) | Signal `low_battery` and power off. The thread calls it after 6 failed voltage reads in a row (60 s, `read_failure_guard`), as the cutoff cannot work; 3 failed reads (30 s) show `fault` once, a good read rearms both. A failed ADC or GPIO setup is retried every period and counts as a failed read, so it ends the same way |
| `sensor_lost()` (accelerometer) | Level still (so motion mode runs at the still speed and the activity code is 0), signal `fault` |
| `report(signal)` (board layer) | Shows the signal unless stealth: `pairing_refused`, `fault` |
| `observe(level)` (accelerometer, once per second, motion mode) | Raw activity level of the last second; may switch the level, see [Motion mode](#motion-mode) |
| `update_settings(settings)` (BLE) | Stores the settings, calls `outputs::settings_changed` (saved to flash and notified by the board), applies the fan mode |
| `configure(settings)` (flash, at boot) | Stores the settings only |

### Settings

`device_settings`, written over BLE and kept in flash under the settings key `gtfog/cfg` (loaded by `app_settings_load()` at the start of `main()`, independent of
Bluetooth, so a `bt_enable` failure keeps lock and stealth, and stealth also hides the power-on signal). Eight bytes (`encode`, `decode_settings`); the 6-byte format of firmware 0.3.1 (byte 5 is the still speed) and the 2-byte format of firmware 0.2.0 (bytes 0 and 1) are still
accepted on writes and in flash, anything else is rejected. A short write changes only the fields it carries and keeps the current values of the others;
a short entry in flash is completed with the defaults (walking 50, running 100, and so on):

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
copy to `ble_settings_changed`, which notifies the characteristic at once (system workqueue) and saves it in flash 2 s after the last change, so bursts
(button auto-repeat in motion mode) become one write and the Bluetooth thread never waits for flash. A save is skipped when the
bytes equal the stored ones, and `power_off()` saves a pending copy in its own thread. Values loaded at boot are not saved again.

### Motion mode

With fan mode 1 (and no transport lock) the accelerometer is powered and read every 200 ms. `activity_classifier` takes the standard deviation of
the acceleration magnitude over each second (100 samples) and reports a raw level: below 80 mg still, below 300 mg walking,
else running (`walk_threshold_milli_g`, `run_threshold_milli_g`; guesses, to calibrate with Debug-tab recordings). The
controller (`observe`) keeps the current level, starting at still: it goes up when the raw level has been above it for 2
consecutive seconds (to the lowest level seen in those seconds), and down, when the raw level has stayed below the
current level for `motion_delay_s` seconds, to the highest level seen in those seconds; a second at the current level restarts the countdown. The fan runs at the target
of the current level (`motion_speed[level]`, bytes 5..7). The buttons and Bluetooth fan writes change the target of the
current level and store the settings, so `fan_speed()` and the fan characteristic are that target (0 under the lock). Manual mode keeps its own
target and ignores the activity; switching mode resets the level to still. If the sensor stays silent (see [Accelerometer](#accelerometer)) the level is forced to still.

## Buttons

Two active-low buttons with pull-ups: minus and plus. A GPIO edge wakes the `buttons` thread; while a button is down it
re-evaluates every 20 ms. Thresholds are `button_timing` defaults in `gestures.hpp`.

| Gesture | Result |
| --- | --- |
| Short press: released after 50 ms up to under 500 ms | Plus: fan up one step (`fan_step`, default 5 %); minus: down one step (reported on release); a release after 500 ms and before the first repeat counts as one step too |
| Shorter than 50 ms | Ignored (bounce) |
| Hold of at least 500 ms | Repeats every 200 ms; steps per repeat = `1 + hold_time / 3 s`, at most 5 |
| Held for at least 30 s | Stuck: ignored until released; the thread stops polling and sleeps until the next pin change |
| Both held, released after 2 s and before 5 s | Strobe toggles, once, on release |
| Both held for 5 s | Power off, while still held (the strobe does not toggle) |
| Three short presses of plus with no minus press between them, the third within 2 s of the first | Third press is `unlock` instead of plus (see [Controller](#controller)) |

A press that is still held when the thread starts (the press that woke the device) triggers no single-button action until both buttons have been released once; holding both from boot still powers off after 5 s. Once both buttons are down together, single-button actions stay off until both are released; letting one go before 2 s cancels the chord; letting one go later toggles the strobe. One action is returned per sample.

## Fan

PWM0 channel 3, 5 ms period, normal polarity. `fan_duty_permille(percent, min_duty)`: 0 % is off; 1..100 % maps
linearly onto `fan_min_duty_permille` (20 %) .. 100 % duty, so a running fan never stalls. `pwm_pulse_ns` computes the
pulse width in 64 bits (5 ms x 1000 overflows 32 bits). In motion mode the fan speed follows the activity, see [Motion mode](#motion-mode).

## Strobe

PWM1 channel 0, 1 ms period, normal polarity. On: full duty for 100 ms, off for 100 ms, repeated by a `k_work`. Off:
the work is cancelled and the duty is 0. A step that is already running checks the wanted state under a mutex, so it cannot switch the strobe back on.

## Status LED

RGB LED on PWM0 channels 0..2 (inverted polarity, 5 ms period: the channels of one PWM instance share a period). All writes to PWM0 (fan and LED) go through `set_pwm` under one mutex. `pattern_of` in `board.cpp` maps a signal to a color
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
| `pairing_refused` | Magenta | 2 | 100 ms / 100 ms |
| `fault` | Red | 4 | 50 ms / 50 ms |
| `power_off` | Cyan | 1 | 300 ms |
| `rf_test` | Blue | 1 | 100 ms, repeated every second during the [RF carrier test](#rf-carrier-test); shown even in stealth mode |

## Battery and charger

- `battery` thread: ADC channel `vbat` (AIN4, P0.28, gain 1/6, 12 bit) times `vbat_divider` (2, a 47k/47k divider on
  the POWER sheet), and the charger STAT1 (P0.03) and STAT2 (P0.02) levels, every 10 s.
- Charger BQ25185 (POWER sheet). `decode_charger_state(stat1, stat2)` per the datasheet status table: both high is not
  charging (charge done, sleep or no input power), STAT1 high and STAT2 low is charging, STAT1 low is a fault (with STAT2
  high recoverable: input overvoltage, TS hot or cold, thermal shutdown, system short; both low latched).
- `battery_percentage(mV)`: linear interpolation of a Li-ion curve: 3300 mV 0 %, 3500 5 %, 3600 10 %, 3700 30 %,
  3800 50 %, 3900 65 %, 4000 80 %, 4200 100 %. Reported through the BLE Battery Service. The charger state also goes to the charger characteristic.
- `battery_guard`: 3 consecutive samples below 3300 mV while the charger is not charging make the
  controller show the red low-battery signal and power off. Any other sample resets the count.
- The `vdd` ADC channel (VDDH / 5) is in the devicetree and not sampled.

## Bluetooth LE

Peripheral, device name `GTFOg`, one connection at a time (`CONFIG_BT_MAX_CONN=1`), ATT MTU 247, data length 251.
GATT caching is on, so bonded phones get Service Changed and rediscover the database after a firmware update.
Advertising data: flags and the control service UUID; the name is in the scan
response. Advertising restarts after a disconnect; when starting it fails, it is retried every 5 s. The image is confirmed only when `bt_enable` succeeded, not when advertising starts.

| Service | UUID | Characteristic | Properties |
| --- | --- | --- | --- |
| GTFOg control | `a7f00001-5c1e-4b8e-9d2a-6f0e1c3b4d50` | Fan speed `a7f00002-...`: uint8, 0..100 | read, write, notify |
| | | Strobe `a7f00003-...`: uint8, 0 or 1 | read, write, notify |
| | | Accelerometer `a7f00004-...`: stream, see below | notify |
| | | Charger `a7f00005-...`: uint8, 0 not charging, 1 charging, 3 fault (2 is no longer sent) | read, notify |
| | | Settings `a7f00006-...`: 8 bytes, see [Settings](#settings) | read, write, notify |
| | | Fan state `a7f00007-...`: 2 bytes, [0] speed the fan runs at in %, [1] activity level 0 still, 1 walking, 2 running, 255 manual mode; notified when either changes | read, notify |
| | | RF test `a7f00008-...`: 2 bytes, [0] BLE channel index 0..39, [1] seconds 1..255; see [RF carrier test](#rf-carrier-test) | write |
| mcumgr SMP | standard SMP UUID | Firmware update, see [Firmware update](#firmware-update-and-mcuboot) | write, notify |
| Battery Service | standard | Battery level, updated every 10 s | per Zephyr BAS |
| Device Information Service | standard | Model number `GTFOg`, manufacturer `elmaczete_customs` | per Zephyr DIS |

The control, charger and SMP characteristics and all CCC descriptors need an encrypted link (Just Works pairing, LE Secure Connections only: `CONFIG_BT_SMP_SC_ONLY`, so a phone without them cannot pair); bonds are stored
in settings (NVS, `storage_partition`). Writes must have the exact length (1 byte, settings 2, 6 or 8, RF test 2); out-of-range values are rejected. Changes made by the
buttons are notified to a connected phone. The settings store holds the device settings and the bonds: up to 4
(`CONFIG_BT_MAX_PAIRED`), and pairing a fifth device replaces the oldest. The firmware has no gesture or command that removes a bond. To clear all bonds, erase the flash over SWD (`west flash --erase`) and flash again; this also removes the settings.

New pairings are accepted only in the first 3 minutes after boot (`pairing_accept` in `ble.cpp`; every wake from System OFF is a boot); later requests are refused with `BT_SECURITY_ERR_PAIR_NOT_ALLOWED`. Bonded phones reconnect at any time. A refused request shows the `pairing_refused` signal (posted by a `k_work`, as the callback must not take the controller mutex). Pairing stays Just Works, but legacy pairing is not accepted.

## Accelerometer

The ADXL345 is accessed over raw I2C (no Zephyr sensor driver, `CONFIG_SENSOR` off). The `accelerometer` thread powers it
only while a phone subscribes to characteristic `a7f00004` or motion mode is on; otherwise the power pin (P1.09) is low.
Each power-up drives the pin low for 100 ms, then high, waits 500 ms, checks the device ID (0xE5) on I2C0 (address 0x53)
and sets 100 Hz, +-16 g full resolution and FIFO stream mode (a failure is retried after 10 s). While active it reads
the FIFO every 200 ms (the 32-sample FIFO holds 320 ms), feeds `activity_classifier` and, in motion mode, `controller::observe` once per second,
and sends notifications while subscribed; the FIFO is flushed on each new subscription. The power is cut when
neither is needed and by `accelerometer_power_off()` before System OFF. INT1 (P0.05) and INT2 (P0.04) are in the
devicetree and unused.

Faults: a poll that fails or finds the FIFO empty counts as unhealthy (`fifo_watchdog`); after 1 s of them the sensor is power-cycled
(`power_up()` again; a failed power-up counts too). The third such recovery in a row calls `controller::sensor_lost()` once (signal
`fault`, level still, logged); the sensor then stays off for 60 s before every later attempt, however often it fails (`still_lost`), until it delivers samples again (10 s after a failed power-up before that). A full FIFO (32 entries, `fifo_overflowed`) is flushed and polling continues; a flush and a new subscription also drop the partial activity window (`activity_classifier::reset`).
The thread owns the power pin: `accelerometer_power_off()` sets a shutdown flag, wakes the thread and waits at most 1 s for it to
drive the pin low, then drives it low itself.

Notification frame (little endian, `pack_accelerometer_samples`): u16 `seq` (starts at 0 per subscription), u8 `rate_hz` (100), u8 `n`,
then `n` x int16 x, y, z in mg (counts x 3.9). `n` is up to 40 at MTU 247 (`accelerometer_samples_per_packet`). A full
controller buffer drops a packet; a gap in `seq` shows it.

## RF carrier test

For bench measurements (spectrum analyzer, SDR, antenna checks) only: the goggles transmit an unmodulated carrier at 0 dBm, so use it where that is allowed (2.4 GHz band, short, at low power, not near other radios).

- Start: the Debug tab of the app writes `[channel, seconds]` to characteristic `a7f00008` (encrypted link; channel 0..39 is the BLE channel index: 37 -> 2402, 38 -> 2426, 39 -> 2480 MHz, data channels 0..10 -> 2404..2424 and 11..36 -> 2428..2478 MHz; seconds 1..255). A wrong length, channel or 0 seconds gets an ATT error; a second request while one is starting gets one too.
- Run (`rf_test` thread, not the Bluetooth callback): 300 ms after the write response it saves a pending settings change, closes the link, stops the Bluetooth stack (`bt_disable`), starts HFXO, sets RADIO to BLE 1 Mbit at the channel's frequency and 0 dBm and triggers TXEN without START, so the transmitter idles in TXIDLE and sends the bare carrier. The fan and strobe are off and stay off (`board_outputs` ignores the controller while the test runs; button gestures are ignored too) and the status LED blinks blue once a second, also in stealth mode. The battery thread keeps feeding the watchdog; auto-off still counts (the idle timer is the controller's), so with a short auto-off or the transport lock (1 minute) the device may power off before the test ends.
- Stop: any button press (after the first second) or the end of the time, then `sys_reboot(SYS_REBOOT_COLD)`, so Bluetooth comes back clean and the app reconnects. A phone cannot stop the test: the link is gone. If the radio does not start the test ends at once with the reboot.
- Why not MPSL release: `mpsl_lib_uninit()` needs `CONFIG_MPSL_DYNAMIC_INTERRUPTS` and stops the clock the kernel runs on. MPSL stays initialised and idle after `bt_disable()` (no protocol stack or timeslot uses RADIO), and the test drives RADIO itself like the NCS `radio_test` sample.

## Power off and wake-up

`board_outputs::power_off()` runs under the controller mutex, so it waits for no work item: it cancels the LED pattern
and plays the rest of it itself with `k_msleep` (the system workqueue may be blocked on the mutex), turns the LED off,
saves a pending settings change (`ble_settings_flush`), cuts the accelerometer power, waits until both buttons are released (at most 10 s), arms the released ones as level-active wake
sources (a button still held after the wait is left out, as it would wake the chip at once; with both held, both are armed and the chip restarts) and calls `sys_poweroff()` (System OFF). A press wakes the chip through a reset. The mutex is held the whole time, so the battery thread cannot feed the watchdog; the worst case is about 13 s (pattern 1.2 s, the longest, `low_battery` 3 x 400 ms; flash 0.1 s, accelerometer 1 s, buttons 10 s). The last feed can be 10 s old, so the margin is 30 s - 10 s - 13 s, about 7 s, and a longer pattern or wait needs it rechecked.

## Watchdog

The nRF hardware watchdog (`wdt0`, Zephyr `wdt` API, `CONFIG_WATCHDOG`) has a 30 s timeout, reset SoC, and pauses while
the debugger halts the CPU. The `battery` thread starts it and feeds it every 10 s, right after it took and released the
controller mutex, so a stuck controller mutex or a dead `battery` thread resets the device (the fan is off after a reset); a hung `buttons` or `accelerometer` thread is not detected.

## Firmware update and MCUboot

USB in the application is gone; on the board USB is for charging, except for the MCUboot recovery below.

- Flash map (`gtfog.dts`): see [hardware.md](hardware.md#flash-map). MCUboot mode is sysbuild
  `SB_CONFIG_MCUBOOT_MODE_SWAP_USING_MOVE` (swap with revert, no scratch partition) with downgrade prevention.
- Update over Bluetooth: mcumgr SMP (`CONFIG_NCS_SAMPLE_MCUMGR_BT_OTA_DFU`) into slot 1. The link must be encrypted
  (`CONFIG_MCUMGR_TRANSPORT_BT_PERM_RW_ENCRYPT`), so pair first.
- Update with `smpmgr` (`pip install smpmgr`):

```sh
smpmgr --ble <address> image upload build/firmware/zephyr/zephyr.signed.bin
smpmgr --ble <address> image state-write <hash of slot 1>   # marks the image for test
smpmgr --ble <address> os reset
```

  `smpmgr upgrade` uploads and resets but does not mark the image, so it does not install it.
- Revert: after the swap the new image runs unconfirmed. `main()` confirms it (`boot_write_img_confirmed`, helper
  `confirm_image`) only when Bluetooth initialized, and either at the end of `main()` (after the battery thread and the watchdog had 1 s to start) or, when the boot battery check finds the cell too low and `check_start` is about to power off, right before that power off, so a low-battery boot does not revert a good update. An unconfirmed image is reverted by MCUboot on the next reset.
- The image version comes from `software/firmware/VERSION`. Downgrade prevention rejects an older or equal
  image, so raise it for every update.
- Recovery: MCUboot has serial recovery over USB CDC ACM (mcumgr protocol, VID:PID `1209:EACD`, manufacturer
  `elmaczete_customs`, product `GTFOg bootloader`). Hold the plus button for more than 1 s (`CONFIG_BOOT_SERIAL_DETECT_DELAY=1000`, so a normal press that wakes the device does not enter recovery) while the device wakes or is plugged in (devicetree
  alias `mcuboot-button0` = `button_plus` in `sysbuild/mcuboot.overlay`), then
  `smpmgr --port /dev/ttyACMx image upload ...`. MCUboot builds this on the legacy USB device stack (as it does for
  CDC ACM recovery on nRF52); only the bootloader has USB. While in recovery the red status LED is on
  (the status hook in `mcuboot_hooks/status.c` lights it by PWM). There is no timeout: leave recovery with the reset
  button or by power-cycling.
- MCUboot status hooks (`software/firmware/mcuboot_hooks/`, linked into the MCUboot image by `sysbuild.cmake`): at
  startup MCUboot turns the fan, the RGB LED and the strobe off; while it installs an update the red strobe blinks at
  5 Hz (hardware PWM, 200 ms period) and stops before the application starts.
- Boards with an MCUboot built for the old flash map need the new MCUboot over SWD once.
- Images are signed with the public MCUboot development key (`root-rsa-2048`) in CI and development builds: anyone can sign an image
  the bootloader accepts. Release: build with `-DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE=/path/outside/repo/key.pem` and flash the new MCUboot
  over SWD once; see [Release signing key](design/build_and_tooling.md#release-signing-key).

## Logging

Off in the application (`CONFIG_LOG=n`, no boot banner); `LOG_*` calls stay in the code. To debug, set `CONFIG_LOG` and
`CONFIG_LOG_BACKEND_RTT` (plus `CONFIG_USE_SEGGER_RTT`) and read it with a J-Link RTT client or `nrfutil`. MCUboot has
logging off.
