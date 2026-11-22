# GTfog - Smart anti-fog system for Airsoft applications

Work in progress - please hold with PCB manufacturing until the tagged release is created.

An open hardware and firmware anti-fog fan controller for airsoft goggles: a small blower fan driven by an nRF52833
board with a Li-ion battery, USB-C and two buttons.

## Features

- Efficient blower fan
- Internal battery with up to 2000 mAh capacity
- USB-C charging; firmware update over Bluetooth (mcumgr SMP, MCUboot swap with revert), USB serial recovery in the bootloader
- Integrated Accelerometer (ADXL345) for dynamic fan speed control (streamed over Bluetooth, the fan is not driven by it yet)
- Built-in Red Strobe light (in case you forgot to bring one with you ;))
- RGB status LED and two buttons (plus, minus) for manual fan control
- Bluetooth Low Energy: fan speed and strobe control, battery level, charger state, accelerometer stream, firmware update (Android app WIP)
- **Every component is open and available for modification (see appropriate LICENSE file)**

## Build and flash

The firmware targets nRF Connect SDK v3.4.0 (Zephyr 4.4.0).

In a new workspace:

```sh
west init -m git@github.com:jmacheta/GTFOg.git --mr dev gtfog-workspace
cd gtfog-workspace && west update
west build -b gtfog GTFOg/software/firmware
```

With an existing NCS v3.4.0 install, from an NCS v3.4.0 toolchain environment (`nrfutil sdk-manager toolchain launch
--ncs-version v3.4.0 --shell`, or the nRF Connect for VS Code terminal):

```sh
west build -b gtfog software/firmware -- -DBOARD_ROOT=$PWD/software
```

Flash over SWD with `west flash` (J-Link or nrfutil). Later updates go over Bluetooth; see
[doc/firmware.md](doc/firmware.md#firmware-update-and-mcuboot). A board with an older MCUboot needs SWD once (the flash map
changed).

Host tests of the hardware-independent logic:

```sh
west twister --testsuite-root GTFOg/software/firmware/tests --platform native_sim/native/64
```

## Documentation

| File | Contents |
| --- | --- |
| [doc/firmware.md](doc/firmware.md) | Firmware behavior: buttons, fan, strobe, LED, battery, BLE, update |
| [doc/android.md](doc/android.md) | Android app (`software/android/`) |
| [doc/hardware.md](doc/hardware.md) | Board summary, pin table, design files |
| [doc/design/](doc/design) | Firmware design principles, code style, build and tooling |

Contributors and coding agents: start with [AGENTS.md](AGENTS.md).

## Licenses

- Hardware (`hardware/`): CERN Open Hardware Licence Version 2 - Strongly Reciprocal, see
  [LICENSE-HARDWARE](LICENSE-HARDWARE).
- Software (`software/`): GNU General Public License Version 3, see [LICENSE-SOFTWARE](LICENSE-SOFTWARE).
