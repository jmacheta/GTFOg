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
| Accelerometer power | P1.09 | GPIO out | High enables the ADXL345; the firmware keeps it low unless a phone streams or motion mode is on (motion mode needs the transport lock off); the accelerometer thread drives it |
| Accelerometer INT1 | P0.05 | GPIO | Unused by the firmware |
| Accelerometer INT2 | P0.04 | GPIO | Unused by the firmware |
| Charger STAT1 | P0.03 | GPIO in | Pull-up |
| Charger STAT2 | P0.02 | GPIO in | Pull-up |
| Minus button | P0.15 | GPIO in | Active low, pull-up |
| Plus button | P0.17 | GPIO in | Active low, pull-up |
| Battery voltage | P0.28 (AIN4) | SAADC ch 4 | Gain 1/6, internal reference, 12 bit; behind a divider of 2 (47k/47k, unverified) |
| Supply voltage | VDDH / 5 | SAADC ch 0 | Gain 1/2, internal reference, 12 bit; not sampled by the firmware |
| USB | D+ / D- | USBD | MCUboot recovery only |
| Reset button | P0.18 | nRESET | Pin reset enabled by `&uicr { gpio-as-nreset; }` in `gtfog.dts`, see [Power](#power) |

## Power

- The firmware sets the GPIO supply level (UICR `REGOUT0`) to 3.3 V on first boot when the chip runs from the high-voltage
  supply (VDDH), then resets once.
- The reset button on P0.18 works only after UICR `PSELRESET` is programmed. The startup code (nrfx, from `gpio-as-nreset` in `gtfog.dts`, in the application and in MCUboot) writes it on the first boot of a new image, also after a Bluetooth update, and a chip reset follows. A chip that was erased or never ran this firmware has the pin as a plain GPIO until then.
- The low-frequency clock is the RC oscillator in the application (no 32 kHz crystal on the board); MCUboot uses the
  synthesized clock.
- Power-off uses System OFF; the buttons wake the chip.
- The charger and regulator parts are in `POWER.SchDoc`; their part numbers are not repeated here (see the BoM).
- Rev 1.0 bug: on USB power alone the fan does not run. The 3.3 V high level on P0.11 is below the input-high threshold
  of the fan driver. With a battery connected the charger pulls the supply down a little and the driver accepts the
  signal. Bench-test the fan with a battery connected.

## Rev 1.0 errata (from the schematic PDF and board measurements, 2026-11-22)

- **Charger TS/MR floating:** U301 pin 6 (TS/MR) is unconnected. The BQ25185 reads that as a temperature fault and
  does not charge (STAT1 low, STAT2 high). Fix: 10 kΩ from TS/MR to GND, or a 10 kΩ NTC on the cell; never tie it
  straight to GND (manual reset / factory mode). Reworked on boards 1 and 2; both then report charging.
- **Board 1 32 MHz crystal off by about +215 ppm** (X201, measured over SWD with a 1 MHz TIMER against the host clock,
  two intervals to cancel the probe overhead). BLE allows ±50 ppm, so the board transmits but nobody decodes it. The
  nRF52833 has no internal load capacitors to trim and the radio has 1 MHz steps only, so firmware cannot correct it:
  replace X201. Board 2 works.
- **Battery divider always on:** R301/R302 (2 × 47 kΩ) draw about 45 µA from the cell, also in System OFF. A switched
  divider or larger resistors would cut it.
- **Strobe:** D402/D403 from VSYS through 68 Ω each (R404/R406), switched low-side by Q401 (2N7002): about 26-29 mA per
  LED, the LEDs' DC limit. Brighter needs other LEDs or a pulsed design, not firmware.
- **Fan:** MCP1643 EN driven straight from P0.11 with a 47 kΩ pull-down (R401); output 176 mA set by R403 (0.68 Ω).
- **ADXL345** is powered from P1.09 directly (VDD_ADXL), so the firmware powers it down when unused.

## Flash map

| Partition | Offset | Size |
| --- | --- | --- |
| `mcuboot` | 0x00000 | 0x10000 (64 KB) |
| `image-0` (slot 0) | 0x10000 | 0x35000 (212 KB) |
| `image-1` (slot 1) | 0x45000 | 0x35000 (212 KB) |
| `storage` | 0x7A000 | 0x06000 (24 KB) |

There is no scratch partition (swap using move). Use: the application (built with LTO) takes about 160 KB of about 204 KB
available, MCUboot about 54 KB of 64 KB. `storage` holds the Bluetooth bonds and the device settings. Boards with an MCUboot built for an older map
need the new MCUboot over SWD once.

## Calibration knobs

Marked `ponytail:` in the code; tune them on a board.

| Knob | Value | Where |
| --- | --- | --- |
| `vbat_divider` | 2 (47k/47k on the POWER sheet, unverified) | `battery.cpp` |
| `fan_min_duty_permille` | 200 | `board.cpp` |
| Li-ion curve | 8 points, 3300..4200 mV | `battery_model.cpp` |
| Charger STAT table | per the BQ25185 datasheet; board 2 reports a recoverable fault on USB (TS network?) | `battery_model.cpp` |

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

- Sheet contents above were first inferred from file names; the errata section is from the schematic PDF export.
- Regulator part number, battery connector and fan model are not stated in the repo text; see the BoM.
- Which signals are on the USB harness beyond D+/D- (VBUS sense?) is not visible in the devicetree.
