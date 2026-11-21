# Hardware

Rev 1.0, designed in Altium Designer (`hardware/GTFOg.PrjPcb`). The binary Altium files are described here by name
only. The Zephyr board definition in `software/boards/arm/gtfog/` is the readable source of truth for pins.

## Summary

| Item | Value |
| --- | --- |
| MCU | nRF52833 (`SOC_NRF52833_QIAA`), 512 KiB flash, 128 KiB RAM |
| Fan | Blower fan driven by PWM (PWM0 channel 3, P0.11) |
| Battery | Li-ion, up to 2000 mAh (README); charger BQ25185 (POWER sheet), status on two pins |
| USB | USB-C: charging; in MCUboot also serial recovery (VID 0x1209, PID 0xEACD); no USB in the application |
| Sensor | ADXL345 accelerometer, I2C0 address 0x53, switched supply pin |
| Lights | RGB status LED (PWM0), red strobe LED (PWM1) |
| Inputs | Two buttons (minus, plus), active low with pull-ups |
| Debug | SWD (`board.cmake`: J-Link, pyocd, nrfjprog, nrfutil, openocd) |

## Pins

From `gtfog.dts` and `gtfog-pinctrl.dtsi`.

| Signal | nRF pin | Peripheral | Notes |
| --- | --- | --- | --- |
| LED red | P0.31 | PWM0 out 0 | Inverted, 5 ms period |
| LED green | P0.30 | PWM0 out 1 | Inverted, 5 ms period |
| LED blue | P0.29 | PWM0 out 2 | Inverted, 5 ms period |
| Fan | P0.11 | PWM0 out 3 | Normal polarity, 5 ms period |
| Strobe | P0.20 | PWM1 out 0 | Normal polarity, 1 ms period |
| I2C SCL | P0.00 | TWIM0 | Standard rate, pull-up |
| I2C SDA | P0.01 | TWIM0 | Standard rate, pull-up |
| Accelerometer power | P1.09 | GPIO out | High enables the ADXL345 |
| Accelerometer INT1 | P0.05 | GPIO | Unused by the firmware |
| Accelerometer INT2 | P0.04 | GPIO | Unused by the firmware |
| Charger STAT1 | P0.03 | GPIO in | Pull-up |
| Charger STAT2 | P0.02 | GPIO in | Pull-up |
| Minus button | P0.15 | GPIO in | Active low, pull-up |
| Plus button | P0.17 | GPIO in | Active low, pull-up |
| Battery voltage | P0.28 (AIN4) | SAADC ch 4 | Gain 1/6, internal reference, 12 bit; behind a divider of 2 (47k/47k, unverified) |
| Supply voltage | VDDH / 5 | SAADC ch 0 | Gain 1/2, internal reference, 12 bit; not sampled by the firmware |
| USB | D+ / D- | USBD | MCUboot recovery only |

## Power

- The firmware sets the GPIO supply level (UICR `REGOUT0`) to 3.3 V on first boot when the chip runs from the high-voltage
  supply (VDDH), then resets once.
- The low-frequency clock is the RC oscillator in the application (no 32 kHz crystal on the board); MCUboot uses the
  synthesized clock.
- Power-off uses System OFF; the buttons wake the chip.
- The charger and regulator parts are in `POWER.SchDoc`; their part numbers are not repeated here (see the BoM).
- Rev 1.0 bug: on USB power alone the fan does not run. The 3.3 V high level on P0.11 is below the input-high threshold
  of the fan driver. With a battery connected the charger pulls the supply down a little and the driver accepts the
  signal. Bench-test the fan with a battery connected.

## Flash map

| Partition | Offset | Size |
| --- | --- | --- |
| `mcuboot` | 0x00000 | 0x10000 (64 KB) |
| `image-0` (slot 0) | 0x10000 | 0x35000 (212 KB) |
| `image-1` (slot 1) | 0x45000 | 0x35000 (212 KB) |
| `storage` | 0x7A000 | 0x06000 (24 KB) |

There is no scratch partition (swap using move). Use: the application (built with LTO) takes about 156 KB of about 204 KB
available, MCUboot about 54 KB of 64 KB. `storage` holds the Bluetooth bonds. Boards with an MCUboot built for an older map
need the new MCUboot over SWD once.

## Calibration knobs

Marked `ponytail:` in the code; tune them on a board.

| Knob | Value | Where |
| --- | --- | --- |
| `vbat_divider` | 2 (47k/47k on the POWER sheet, unverified) | `battery.cpp` |
| `fan_min_duty_permille` | 200 | `board.cpp` |
| Li-ion curve | 8 points, 3300..4200 mV | `logic/battery.cpp` |
| Charger STAT table | unverified against the BQ25185 datasheet | `logic/battery.cpp` |

## Design files (`hardware/`)

| File | Contents |
| --- | --- |
| `GTFOg.PrjPcb`, `GTFOg.PrjPcbStructure` | Altium project and its structure cache |
| `Main.SchDoc` | Top-level sheet that ties the sheets below together |
| `nRF52833.SchDoc`, `nRF52833.Harness` | MCU sheet and its signal harness |
| `POWER.SchDoc` | Power: battery, charging, regulation |
| `USB.SchDoc`, `USB.Harness` | USB-C connector and USB signals |
| `SENSORS.SchDoc`, `SENSORS.Harness` | Sensors (accelerometer) |
| `PERIPHERALS.SchDoc`, `PERIPHERALS.Harness` | Fan, LEDs, strobe, buttons (by name; check the sheet) |
| `CHANGELOG.SchDoc` | Revision notes sheet |
| `PCB.PcbDoc` | Board layout |
| `PCB_3D.step` | 3D model |

## Manufacturing outputs

| File | Contents |
| --- | --- |
| `GTFOg_gerber.zip` | Gerber files |
| `GTFOg_Rev1.0_BoM.xlsx` | Bill of materials, Rev 1.0 |

The README asks to hold with PCB manufacturing until a tagged release exists.

## Open questions

- Sheet contents above are inferred from file names, not read from the Altium files.
- Regulator part number, battery connector and fan model are not stated in the repo text; see the BoM.
- Which signals are on the USB harness beyond D+/D- (VBUS sense?) is not visible in the devicetree.
