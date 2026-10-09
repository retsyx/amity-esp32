#!/usr/bin/env python3
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

"""Probe Matter's unencrypted BTP capability exchange through BlueZ.

Run on the authorized Pi with the project-local dbus-next wheel in PYTHONPATH.
This scans briefly, connects to one explicitly selected device, writes the
standard BTP v4 capability request, subscribes, then disconnects. It does not
pair, change bonds, or alter Wi-Fi or Matter fabrics.
"""

import argparse
import asyncio
import contextlib
import json

from dbus_next import BusType, Variant
from dbus_next.aio import MessageBus

BLUEZ = "org.bluez"
PROPS = "org.freedesktop.DBus.Properties"
RX = "18ee2ef5-263d-4559-959f-4f9c429f9d11"
TX = "18ee2ef5-263d-4559-959f-4f9c429f9d12"


async def main(args):
    bus = await MessageBus(bus_type=BusType.SYSTEM).connect()

    async def proxy(path):
        return bus.get_proxy_object(BLUEZ, path, await bus.introspect(BLUEZ, path))

    manager = (await proxy("/")).get_interface("org.freedesktop.DBus.ObjectManager")
    adapter = (await proxy("/org/bluez/hci0")).get_interface("org.bluez.Adapter1")
    device_path = "/org/bluez/hci0/dev_" + args.address.upper().replace(":", "_")
    device = None
    scanning = False
    indications = []
    try:
        await adapter.call_set_discovery_filter({"Transport": Variant("s", "le")})
        await adapter.call_start_discovery()
        scanning = True
        for _ in range(args.scan_seconds * 5):
            objects = await manager.call_get_managed_objects()
            props = objects.get(device_path, {}).get("org.bluez.Device1", {})
            if "RSSI" in props:
                print(json.dumps({"address": args.address, "rssi": props["RSSI"].value}), flush=True)
                break
            await asyncio.sleep(0.2)
        else:
            raise RuntimeError("Device was not discovered")
        await adapter.call_stop_discovery()
        scanning = False
        obj = await proxy(device_path)
        device = obj.get_interface("org.bluez.Device1")
        props = obj.get_interface(PROPS)
        await asyncio.wait_for(device.call_connect(), 20)
        for _ in range(100):
            if (await props.call_get("org.bluez.Device1", "ServicesResolved")).value:
                break
            await asyncio.sleep(0.1)
        else:
            raise RuntimeError("Service discovery timed out")
        print(json.dumps({"connected": True, "paired":
                          (await props.call_get("org.bluez.Device1", "Paired")).value}), flush=True)
        chars = {}
        for path, interfaces in (await manager.call_get_managed_objects()).items():
            char = interfaces.get("org.bluez.GattCharacteristic1")
            if path.startswith(device_path + "/") and char and char["UUID"].value in (RX, TX):
                chars[char["UUID"].value] = path
                print(json.dumps({"uuid": char["UUID"].value, "flags": char["Flags"].value}), flush=True)
        rx_obj, tx_obj = await proxy(chars[RX]), await proxy(chars[TX])
        tx = tx_obj.get_interface("org.bluez.GattCharacteristic1")

        def changed(interface, changes, _invalidated):
            if interface == "org.bluez.GattCharacteristic1" and "Value" in changes:
                value = bytes(changes["Value"].value).hex()
                indications.append(value)
                print(json.dumps({"indication": value}), flush=True)

        tx_obj.get_interface(PROPS).on_properties_changed(changed)
        # Match CHIP central: write first to create the BLE endpoint, then
        # subscribe, which prompts the peripheral's capability response.
        await rx_obj.get_interface("org.bluez.GattCharacteristic1").call_write_value(
            bytes.fromhex("656c04000000000005"), {"type": Variant("s", "request")})
        print(json.dumps({"capability_write": "accepted"}), flush=True)
        await tx.call_start_notify()
        print(json.dumps({"subscription": "accepted"}), flush=True)
        await asyncio.sleep(args.wait_seconds)
        print(json.dumps({"indications": indications}), flush=True)
    finally:
        if device:
            with contextlib.suppress(Exception):
                await device.call_disconnect()
        if scanning:
            with contextlib.suppress(Exception):
                await adapter.call_stop_discovery()
        bus.disconnect()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("address")
    parser.add_argument("--scan-seconds", type=int, default=20)
    parser.add_argument("--wait-seconds", type=float, default=5)
    asyncio.run(main(parser.parse_args()))
