# Firmware

Zephyr C++ application in `software/firmware`, built with MCUboot through sysbuild. Design rules are in
[design/principles.md](design/principles.md); build steps in [design/build_and_tooling.md](design/build_and_tooling.md).
Everything below describes the code as it is; unclear points are in [Open questions](#open-questions).

## Modules

Pure logic, no Zephyr includes (`src/logic/`, tested on the host):

| File | Contents |
| --- | --- |
| `buttons.hpp/.cpp` | `button_gestures`: turns sampled button levels and time into `button_event`s; `button_timing` holds the thresholds |
| `battery.hpp/.cpp` | `decode_charger_state`, `battery_percentage` (Li-ion curve), `battery_guard` (low-battery cutoff) |
| `controller.hpp/.cpp` | `controller`: fan speed, strobe, signals, power off; abstract `outputs`; `fan_duty_permille` |

Board layer (`src/`):

| File | Contents |
| --- | --- |
| `app.hpp` | `with_controller(f)`: runs `f(controller &)` under a mutex; declarations shared by the board files |
| `board.cpp` | `board_outputs` (implements `outputs`): PWM fan, PWM strobe and RGB LED patterns (`k_work`), System OFF; owns the `controller` |
| `buttons.cpp` | Button GPIO interrupts, `buttons` thread feeding `button_gestures`, wake-up arming |
| `battery.cpp` | `battery` thread: samples VBAT and the charger STAT pins every 10 s |
| `ble.cpp` | Advertising, GATT control service, Battery Service, Device Information Service |
| `usb.cpp` | USB device with the DFU class; writes a new image to slot 1 |
| `accelerometer.cpp` | `accelerometer` thread: powers the ADXL345 and logs samples |
| `main.cpp` | `main()`: `power_on`, `usb_init`, `ble_init`; `SYS_INIT` hook that sets the GPIO supply (UICR `REGOUT0`) to 3.3 V and resets once if it is not |

## Threads

| Thread | Priority | Stack | Job |
| --- | --- | --- | --- |
| `main` | Zephyr default | 2048 B (`CONFIG_MAIN_STACK_SIZE`) | Powers the controller on, starts USB and BLE, returns |
| `buttons` | 5 | 1024 B | Waits for a pin change; polls every 20 ms while a button is down |
| `battery` | 7 | 1024 B | Starts after 500 ms; samples every 10 s |
| `accelerometer` | 10 | 1024 B | Starts after 100 ms; samples once per second |

The strobe, the LED patterns, BLE notifications, advertising restart and the USB reboot run on the system workqueue
(2048 B stack). Every thread reaches the controller through `with_controller`.

## Controller

`controller` (`src/logic/controller.cpp`) holds the fan speed (0..100 %), the strobe state and the battery percentage,
and drives an `outputs` object.

| Input | Effect |
| --- | --- |
| `power_on()` | Signal `power_on`, fan 0, strobe off |
| Button `plus` / `minus` | Fan speed `+steps` / `-steps`, clamped to 0..100; signal `speed_up`, `speed_down`, or `speed_limit` when the result reaches 0 or 100 |
| Button `toggle_strobe` | Strobe on or off |
| Button `power_off` | Strobe off, fan 0, `outputs::power_off()` |
| `set_fan_speed(percent)` (BLE) | Fan speed, clamped to 100; no signal |
| `set_strobe(on)` (BLE) | Strobe on or off; no signal |
| `battery_status` | Stores the percentage; when `battery_guard` fires: signal `low_battery`, then the same as `power_off` |

## Buttons

Two active-low buttons with pull-ups: minus and plus. A GPIO edge wakes the `buttons` thread; while a button is down it
re-evaluates every 20 ms. Thresholds are `button_timing` defaults in `buttons.hpp`.

| Gesture | Result |
| --- | --- |
| Short press: released after 50 ms up to under 500 ms | Plus: fan +1; minus: fan -1 (reported on release) |
| Shorter than 50 ms | Ignored (bounce) |
| Hold of at least 500 ms | Repeats every 200 ms; steps per repeat = `1 + hold_time / 3 s`, at most 5 |
| Held for at least 30 s | Stuck: ignored until released |
| Both held for 2 s | Strobe toggles, once |
| Both held for 5 s | Power off (the strobe toggled at 2 s is turned off with it) |

Once both buttons are down together, single-button actions stay off until both are released; letting one go before a
threshold cancels the chord. One action is returned per sample.

## Fan

PWM0 channel 3, 5 ms period, normal polarity. `fan_duty_permille(percent, min_duty)`: 0 % is off; 1..100 % maps
linearly onto `fan_min_duty_permille` (20 %) .. 100 % duty, so a running fan never stalls. The accelerometer does not
control the fan.

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
| Power off | Cyan, steady | | 300 ms, after the buttons are released |

## Battery and charger

- `battery` thread: ADC channel `vbat` (AIN4, P0.28, gain 1/6, 12 bit) times `vbat_divider` (2, a 47k/47k divider on
  the POWER sheet), and the charger STAT1 (P0.03) and STAT2 (P0.02) levels, every 10 s.
- Charger BQ25185 (POWER sheet). `decode_charger_state(stat1, stat2)`: both high charge completed, STAT1 only charging,
  STAT2 only fault, both low not charging.
- `battery_percentage(mV)`: linear interpolation of a Li-ion curve: 3300 mV 0 %, 3500 5 %, 3600 10 %, 3700 30 %,
  3800 50 %, 3900 65 %, 4000 80 %, 4200 100 %. Reported through the BLE Battery Service.
- `battery_guard`: 3 consecutive samples below 3300 mV while the charger is neither charging nor completed make the
  controller show the red low-battery signal and power off. Any other sample resets the count.
- The `vdd` ADC channel (VDDH / 5) is in the devicetree and not sampled.

## Bluetooth LE

Peripheral, device name `GTFOg`. Advertising data: flags and the control service UUID; the name is in the scan
response. Advertising restarts after a disconnect.

| Service | UUID | Characteristic | Properties |
| --- | --- | --- | --- |
| GTFOg control | `a7f00001-5c1e-4b8e-9d2a-6f0e1c3b4d50` | Fan speed `a7f00002-...`: uint8, 0..100 | read, write, notify |
| | | Strobe `a7f00003-...`: uint8, 0 or 1 | read, write, notify |
| Battery Service | standard | Battery level, updated every 10 s | per Zephyr BAS |
| Device Information Service | standard | Model number `GTFOg`, manufacturer `elmaczete_customs` | per Zephyr DIS |

The control characteristics and their CCC descriptors need an encrypted link (Just Works pairing); bonds are stored
in settings (NVS, `storage_partition`). Writes must be one byte; out-of-range values are rejected. Changes made by the
buttons are notified to a connected phone. The settings store holds only the bonds.

## Accelerometer

The `accelerometer` thread drives the ADXL345 power pin (P1.09) low for 100 ms, then high, waits 500 ms, initializes the
deferred-init device on I2C0 (address 0x53), sets 100 Hz sampling, and logs X, Y, Z (m/s^2) every second at debug
level. INT1 (P0.05) and INT2 (P0.04) are in the devicetree and unused.

## Power off and wake-up

`board_outputs::power_off()` shows cyan, waits until both buttons are released (at most 10 s), arms both as
level-active wake sources, waits 300 ms, turns the LED off and calls `sys_poweroff()` (System OFF). A press wakes the
chip through a reset.

## USB DFU and MCUboot

- Flash map (`gtfog.dts`): see [hardware.md](hardware.md#flash-map). MCUboot is upgrade only with downgrade
  prevention.
- USB device_next. The run-time device (VID:PID `1209:EACC`, manufacturer `elmaczete_customs`, product `GTFOg`) has the
  DFU run-time class `dfu_runtime`. A detach request makes the firmware shut that device down and enumerate again as
  `1209:EACD` with the DFU class, whose alternate setting is `slot1_image` (flash slot 1).
- After a completed download the firmware calls `boot_request_upgrade(BOOT_UPGRADE_PERMANENT)` and reboots 1 s later;
  MCUboot installs the image.
- Update: `dfu-util -d 1209:eacc,1209:eacd -a slot1_image -D build/firmware/zephyr/zephyr.signed.bin`.
- The device is enabled on VBUS ready and disabled on VBUS removal.
- The image version comes from `software/firmware/VERSION`. Downgrade prevention rejects an older image, so bump it for
  every release.
- MCUboot status hooks (`software/firmware/mcuboot_hooks/`, linked into the MCUboot image by `sysbuild.cmake`): at
  startup MCUboot turns the fan, the RGB LED and the strobe off; while it installs an update the red strobe blinks at
  5 Hz (hardware PWM, 200 ms period) and stops before the application starts. Boards that only get USB updates keep
  the MCUboot they shipped with; a new MCUboot needs SWD.
- Linux access without root: `SUBSYSTEM=="usb", ATTR{idVendor}=="1209", ATTR{idProduct}=="eac[cd]", MODE="0666"` in
  `/etc/udev/rules.d/70-gtfog.rules`.

## Logging

`CONFIG_LOG` with the RTT backend, deferred mode. Application modules log at INF (`LOG_MODULE_REGISTER` per file), the
rest at warning. Read it with a J-Link RTT client or `nrfutil`. The MCUboot log level is warning.

## Open questions

- Accelerometer-driven fan control is not designed.
- The charger STAT table and the VBAT divider need checking on a board.
- BLE: on the first board the radio transmits on the advertising channels (RADIO in TX, checked over SWD, also at
  +8 dBm), but no host receives it. Suspect the RF path (antenna, matching) or the 32 MHz crystal, not the firmware.
- Flash headroom is about 2 KB (see [hardware.md](hardware.md#flash-map)).
