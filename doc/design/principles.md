# Project principles

What the firmware in `software/firmware` has to satisfy. Behavior is in [firmware.md](../firmware.md).

## Goals

- **Small and predictable.** A battery-powered nRF52833 (512 KiB flash, 128 KiB RAM, of which an application slot is
  212 KB). No dynamic allocation in application code; threads and stacks are static (`K_THREAD_DEFINE`). The application
  uses about 160 KB of about 204 KB, so features still have to pay for themselves.
- **Modern C++.** C++23 (`CONFIG_STD_CPP23`), `std::chrono` for time values in the logic's interfaces (plain integers only for wire fields and Zephyr tick macros, with the unit in the name), `constexpr` where it helps, no
  exceptions, no RTTI.
- **Safe in the field.** The fan and strobe never depend on a radio or a phone: manual control works without BLE.
  Power-off by both buttons is reachable from every state.
- **Updatable.** The image is signed and swapped by MCUboot (swap with revert, downgrade prevention); updates go over Bluetooth, and the image confirms itself only once Bluetooth
  works (at the end of `main`, or right before a low-battery power off at boot).

## Structure

- **Pure logic apart from the board.** `gestures`, `battery_model`, `motion` and `controller` (headers in `src/include/`,
  sources in `src/`, listed in `src/logic.cmake`) have no Zephyr includes and are built twice: into the firmware and into
  the host tests. They hold the button gesture detector (`button_gestures`), the battery functions
  (`decode_charger_state`, `battery_percentage`, `battery_guard`), the accelerometer packet
  (`pack_accelerometer_samples`) and `activity_classifier`, and the `controller` with `fan_duty_permille` and `pwm_pulse_ns`.
- **Behavior goes into the pure logic and gets a test; board files stay thin.** The board layer in `src/` only reads
  hardware, passes values to the logic and carries out its outputs.
- **One controller behind a mutex, as one state machine.** The controller is a Boost.SML transition table
  (`controller_table`): inputs (buttons, BLE writes, battery samples) call `controller` methods through
  `with_controller` (`app.hpp`), which turn them into events; actions run in the calling thread. New behavior is a new
  row, guard or action in that table, with the computing done in plain functions. The controller drives hardware only through the
  abstract `outputs` interface, implemented by `board_outputs` in `board.cpp` (PWM fan, strobe and RGB LED, System OFF)
  and by a fake in the tests.
- **Never wait for the system workqueue while holding the mutex.** BLE notifications take the mutex on the workqueue, so
  an output that waits for a work item deadlocks; `outputs` methods do their timing in the calling thread or hand off
  work without blocking (`settings_changed`).
- **One thread per sampled input.** `buttons`, `battery` and `accelerometer` are static threads; timed output
  (strobe, LED patterns) runs as `k_work` on the system workqueue. `main` starts BLE, checks the battery (confirming the
  image first if that check powers off), powers the controller on, confirms the image and returns.
- **Pins and peripherals live in the board definition.** Application code reads GPIOs, PWM, ADC and I2C through
  devicetree labels and the `zephyr,user` node, so a pin change is a change in `gtfog.dts` only.
- **Configuration** is `constexpr` values next to the code that uses them and Kconfig. Settings (NVS in
  `storage_partition`) hold the Bluetooth bonds and the device settings (`gtfog/cfg`).
- **Errors** in the board layer are negative errno ints; the logic has no failure paths.
- **Deliberate simplifications and calibration constants** carry a `ponytail:` comment naming the ceiling or what to
  measure.
- **One third-party library** beyond Zephyr and the C++ standard library: header-only Boost.SML for the controller,
  fetched by CMake at a pinned version and hash.

## Out of scope

- A bootloader of our own: MCUboot comes from NCS.
- A fuel gauge: the battery percentage is a voltage curve (see [firmware.md](../firmware.md#battery-and-charger)).
- A C API or a generic HAL: the code targets this one board.
- Unit tests of hardware access: only the pure logic is tested.
