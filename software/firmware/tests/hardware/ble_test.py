#!/usr/bin/env python3
"""Bluetooth LE check against a powered GTFOg board: python3 ble_test.py [--name GTFOg].

Needs `pip install bleak` and a Bluetooth adapter on the host. On Linux, pair once first, since BlueZ refuses Just Works
without an agent: `bluetoothctl` -> `agent NoInputNoOutput`, `default-agent`, `scan le`, `pair <address>`, `trust <address>`.
Then the test exercises the control service,
the Battery Service and the Device Information Service. Leaves the fan and the strobe off.
"""

import argparse
import asyncio
import sys

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

FAN = "a7f00002-5c1e-4b8e-9d2a-6f0e1c3b4d50"
STROBE = "a7f00003-5c1e-4b8e-9d2a-6f0e1c3b4d50"
BATTERY_LEVEL = "00002a19-0000-1000-8000-00805f9b34fb"
MODEL_NUMBER = "00002a24-0000-1000-8000-00805f9b34fb"

failures = []


def check(condition, what):
    print(("PASS " if condition else "FAIL ") + what)
    if not condition:
        failures.append(what)


async def rejected(client, uuid, value):
    try:
        await client.write_gatt_char(uuid, value, response=True)
    except BleakError:
        return True
    return False


async def main(name):
    device = await BleakScanner.find_device_by_name(name, timeout=15)
    check(device is not None, f"advertises as {name}")
    if device is None:
        return

    async with BleakClient(device) as client:
        try:
            await client.pair()
        except (BleakError, NotImplementedError) as e:
            print(f"note: pair() raised {e!r}; continuing, the stack may pair on demand")

        model = (await client.read_gatt_char(MODEL_NUMBER)).decode()
        check(model == "GTFOg", f"DIS model number is GTFOg (got {model!r})")

        level = (await client.read_gatt_char(BATTERY_LEVEL))[0]
        check(0 <= level <= 100, f"battery level in 0..100 (got {level})")

        notified = asyncio.Queue()
        await client.start_notify(FAN, lambda _, data: notified.put_nowait(data[0]))

        async def notified_value(expected):
            # BlueZ may also deliver a stale cached value, so wait for the expected one.
            try:
                while (await asyncio.wait_for(notified.get(), 2)) != expected:
                    pass
                return True
            except asyncio.TimeoutError:
                return False

        for speed in (0, 1, 50, 100, 0):
            while not notified.empty():
                notified.get_nowait()
            await client.write_gatt_char(FAN, bytes([speed]), response=True)
            got = (await client.read_gatt_char(FAN))[0]
            check(got == speed, f"fan speed {speed} reads back (got {got})")
            check(await notified_value(speed), f"fan speed {speed} notified")
        await client.stop_notify(FAN)

        check(await rejected(client, FAN, bytes([101])), "fan speed 101 is rejected")
        check(await rejected(client, FAN, bytes([1, 2])), "two-byte fan write is rejected")

        for on in (1, 0):
            await client.write_gatt_char(STROBE, bytes([on]), response=True)
            got = (await client.read_gatt_char(STROBE))[0]
            check(got == on, f"strobe {on} reads back (got {got})")
        check(await rejected(client, STROBE, bytes([2])), "strobe 2 is rejected")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", default="GTFOg")
    asyncio.run(main(parser.parse_args().name))
    print(f"{len(failures)} failure(s)")
    sys.exit(1 if failures else 0)
