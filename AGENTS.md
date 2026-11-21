# AGENTS.md

An open hardware and Zephyr C++ firmware project: a smart anti-fog fan controller for airsoft goggles (nRF52833, blower
fan, Li-ion battery, USB-C, ADXL345, strobe, RGB status LED, BLE). Read [README.md](README.md) first for the
user-facing model.

## Where things are

| Path | Contents |
| --- | --- |
| `hardware/` | Altium Designer project Rev 1.0 (`*.SchDoc`, `PCB.PcbDoc`, harnesses, BoM, Gerbers, 3D model); binary, not readable as text |
| `software/boards/arm/gtfog/` | Board definition: `gtfog.dts`, `gtfog-pinctrl.dtsi` (source of truth for pins), `gtfog_defconfig`, `board.yml` |
| `software/firmware/src/logic/` | Pure C++ logic without Zephyr: gestures, battery, controller; `CMakeLists.txt` lists the files by hand |
| `software/firmware/src/` | Board layer, one `.cpp` per module (`board`, `buttons`, `battery`, `ble`, `usb`, `accelerometer`, `main`), `app.hpp`; `CMakeLists.txt` lists them by hand |
| `software/firmware/tests/logic/` | Ztest suites of `src/logic`, run on `native_sim` |
| `software/firmware/prj.conf`, `sysbuild.conf`, `sysbuild/mcuboot.conf` | Kconfig of the app, of sysbuild and of MCUboot |
| `west.yml`, `zephyr/module.yml` | West manifest (NCS v3.4.0) and the module file that sets `board_root` |
| `.github/workflows/` | CI: firmware build, tests, clang-format check |
| `doc/` | Documentation |

## Commands

Inside a west workspace made from `west.yml` (see [build_and_tooling.md](doc/design/build_and_tooling.md)):

```sh
west build -b gtfog GTFOg/software/firmware     # build firmware and MCUboot (sysbuild)
west build -b gtfog GTFOg/software/firmware -p  # pristine build
west flash                                      # flash over SWD
west twister --testsuite-root GTFOg/software/firmware/tests --platform native_sim/native/64  # host tests
```

From an NCS install, run twister with the path to `software/firmware/tests`.

- Behavior goes into `src/logic` with a test; board files stay thin.
- Hardware behavior cannot be checked without a board: say so instead of claiming it works.

## Before finishing

- Build the firmware.
- Run the tests.
- Run `clang-format -i` on changed C++ (`.clang-format` at the repo root).
- Keep [doc/firmware.md](doc/firmware.md) and [doc/hardware.md](doc/hardware.md) true when behavior, pins or
  configuration change.
- Do not commit unless asked. Never add `Co-Authored-By` or "Generated with" lines to commits or PRs.

## Documents

| File | Read it when |
| --- | --- |
| [doc/design/principles.md](doc/design/principles.md) | Adding or changing a module, thread or controller behavior. |
| [doc/design/code_style.md](doc/design/code_style.md) | Writing or reviewing code. |
| [doc/design/build_and_tooling.md](doc/design/build_and_tooling.md) | Touching the build, Kconfig, MCUboot, tests, CI or formatting. |
| [doc/firmware.md](doc/firmware.md) | Changing or explaining behavior: buttons, fan, strobe, LED, battery, BLE, DFU. |
| [doc/hardware.md](doc/hardware.md) | Touching the board definition, pins or the Altium files. |
