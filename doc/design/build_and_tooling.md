# Build and tooling

## Toolchain

- nRF Connect SDK (NCS) v3.4.0, which is Zephyr 4.4.0; Zephyr SDK 1.0.1 (`arm-zephyr-eabi`).
- Board `gtfog`, qualifier `gtfog/nrf52833` (Zephyr HWMv2). The board root is `software`.
- C++23 (`CONFIG_STD_CPP23`).
- Sysbuild with MCUboot: `sysbuild.conf` has `SB_CONFIG_BOOTLOADER_MCUBOOT=y`; `sysbuild/mcuboot.conf` selects
  upgrade only (`CONFIG_BOOT_UPGRADE_ONLY`), downgrade prevention, warning log level and the synthesized 32 kHz clock.

## West workspace

The repo has `west.yml` (imports `sdk-nrf` v3.4.0, self path `GTFOg`) and `zephyr/module.yml` (`board_root: software`).

```sh
west init -m git@github.com:jmacheta/GTFOg.git --mr dev gtfog-workspace
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

Ztest suites in `software/firmware/tests/logic` (`battery`, `buttons`, `controller`) cover `src/logic`; the test
`CMakeLists.txt` compiles that directory directly. They run on the host with twister:

```sh
west twister --testsuite-root GTFOg/software/firmware/tests --platform native_sim/native/64
```

From an NCS install, pass the path to `software/firmware/tests`. The test case is `gtfog.logic` (`testcase.yaml`).

## Flash and DFU

- `west flash` programs both images over SWD (J-Link or nrfutil; `board.cmake` also lists nrfjprog, pyocd and openocd).
- USB DFU with `dfu-util`: see [firmware.md](../firmware.md#usb-dfu-and-mcuboot).

## CMake and Kconfig

- `software/firmware/CMakeLists.txt` finds Zephyr and adds `src/`; `src/CMakeLists.txt` lists the board sources and adds
  `src/logic/`. New sources are added by hand.
- No third-party libraries are fetched.
- App options are in `prj.conf` (C++23, full libc++, no exceptions or RTTI, RTT logging, PWM, I2C, ADXL345, ADC, NVS
  and settings, Bluetooth peripheral with SMP, BAS and DIS, USB device_next with the DFU class into slot 1).
- Board options are in `gtfog_defconfig` and `Kconfig.gtfog`.

## Formatting

- C++: `clang-format -i` (`.clang-format` at the repo root: LLVM base, 150 columns).
- CMake: `cmake-format -i` with `software/firmware/.cmake-format.json` (120 columns).

## CI

`.github/workflows/build.yml`:

- `firmware`: sets up Zephyr with `zephyrproject-rtos/action-zephyr-setup` (SDK chosen automatically), runs
  `west build`, runs twister on `native_sim/native/64`, and uploads the images and the twister report as artifacts.
- `format`: checks the tracked `.c`, `.h`, `.cpp` and `.hpp` files under `software` with the pinned clang-format 21.1.8.
