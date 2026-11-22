# Build and tooling

## Toolchain

- nRF Connect SDK (NCS) v3.4.0, which is Zephyr 4.4.0; Zephyr SDK 1.0.1 (`arm-zephyr-eabi`).
- Board `gtfog`, qualifier `gtfog/nrf52833` (Zephyr HWMv2). The board root is `software`.
- C++23 (`CONFIG_STD_CPP23`).
- Sysbuild with MCUboot: `sysbuild.conf` has `SB_CONFIG_BOOTLOADER_MCUBOOT=y`; sets `SB_CONFIG_MCUBOOT_MODE_SWAP_USING_MOVE` (swap with
  revert). `sysbuild/mcuboot.conf` selects no downgrade prevention (the app asks before an older image), no logging, the synthesized 32 kHz clock, the status
  hooks and USB serial recovery (CDC ACM, legacy USB device stack); `sysbuild/mcuboot.overlay` sets the recovery button
  (`mcuboot-button0` = plus) and the CDC ACM device.

## West workspace

The repo has `west.yml` (imports `sdk-nrf` v3.4.0, self path `GTFOg`) and `zephyr/module.yml` (`board_root: software`).

```sh
west init -m git@github.com:jmacheta/GTFOg.git --mr main gtfog-workspace
cd gtfog-workspace && west update
west build -b gtfog GTFOg/software/firmware
```

With an existing NCS v3.4.0 install, from an NCS v3.4.0 toolchain environment (`nrfutil sdk-manager toolchain launch
--ncs-version v3.4.0 --shell`, or the nRF Connect for VS Code terminal), run from the repo root:

```sh
west build -b gtfog software/firmware -- -DBOARD_ROOT=$PWD/software
```

Images land in `build/` (the application under `build/firmware/`, MCUboot under `build/mcuboot/`).

## Tests

Ztest suites in `software/firmware/tests/logic` (`motion`, `battery_model`, `gestures`, `controller`, `controller_machine`) cover the pure logic (the Bluetooth and flash glue in `ble.cpp` is Zephyr code and has no host test); the test
`CMakeLists.txt` includes `src/logic.cmake`, the same list the application uses. They run on the host with twister:

```sh
west twister --testsuite-root GTFOg/software/firmware/tests --platform native_sim/native/64
```

From an NCS install, pass the path to `software/firmware/tests`. The test case is `gtfog.logic` (`testcase.yaml`).

On a board: `software/firmware/tests/hardware/ble_test.py` (Python, `pip install bleak`, a Bluetooth adapter) pairs with
the device and checks the control service, BAS and DIS; it runs the fan at 1, 50 and 100 %. It does not cover the accelerometer
stream or the charger characteristic (the stream was checked ad hoc). It is not part of CI. On
Linux, pair once with `bluetoothctl` and a `NoInputNoOutput` agent first (see the script header): without an agent,
BlueZ rejects Just Works pairing (SMP reason 0x0c). The
update check is the `smpmgr` sequence of [firmware.md](../firmware.md#firmware-update-and-mcuboot) with an image whose
`VERSION` is higher than the installed one; the application has no log, so check the result by the new behavior or
by `smpmgr image state-read`.

## Flash and DFU

- `west flash` programs both images over SWD (J-Link or nrfutil; `board.cmake` also lists nrfjprog, pyocd and openocd).
- Updates over Bluetooth with `smpmgr`, and USB serial recovery: see
  [firmware.md](../firmware.md#firmware-update-and-mcuboot).

## CMake and Kconfig

- `software/firmware/CMakeLists.txt` finds Zephyr and adds `src/`; `src/CMakeLists.txt` lists the board sources and includes
  `src/logic.cmake`, which lists the pure logic sources once (`gtfog_add_logic`) for the application and the tests. New
  sources are added by hand.
- Boost.SML (header only, v1.1.11) is the one third-party library: `cmake/sml.cmake` fetches it with `FetchContent` from a
  pinned GitHub URL with a `URL_HASH`, so the first configure needs network access. It builds with `-fno-exceptions
  -fno-rtti` (SML detects both) and is included as a system header.
- App options are in `prj.conf` (C++23, full libc++, no exceptions or RTTI, logging off (RTT backend to be enabled for
  debugging), PWM, I2C, ADC, NVS and settings, Bluetooth peripheral with SMP, BAS and DIS, one connection, MTU 247,
  mcumgr over Bluetooth into slot 1). No sensor driver: the ADXL345 uses raw I2C.
- Board options are in `gtfog_defconfig` and `Kconfig.gtfog`.

## Release signing key

MCUboot uses the public development key (`root-rsa-2048`) for CI and development builds; anyone can sign an image for it. Release
builds select a private key the standard sysbuild way (the key stays outside the repository):

```sh
west build -b gtfog GTFOg/software/firmware -p -- -DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE=/path/outside/repo/key.pem
```

The bootloader carries the public half of the key, so every board needs the new MCUboot flashed over SWD once (`west flash`);
after that, updates over Bluetooth must be signed with the same key. Pairing is Just Works (LE Secure Connections only) in the first 3 minutes after a wake, so
the key is the only protection of the update path: keep it private.

## Formatting

- C++: `clang-format -i` (`.clang-format` at the repo root: LLVM base, 150 columns).
- CMake: `cmake-format -i` with `software/firmware/.cmake-format.json` (120 columns).

## CI

`.github/workflows/build.yml`:

- `firmware`: sets up Zephyr with `zephyrproject-rtos/action-zephyr-setup` (SDK chosen automatically), runs
  `west build`, runs twister on `native_sim/native/64`, and uploads the images and the twister report as artifacts.
- `format`: checks the tracked `.c`, `.h`, `.cpp` and `.hpp` files under `software` with the pinned clang-format 21.1.8.
