#!/usr/bin/env python3
"""BlueZ BLE HID test peripheral; run as root on the authorized Pi harness.

Dependency: dbus-next==0.2.3 (MIT), installed under a project-local PYTHONPATH.
This adds a GATT application/advertisement and temporarily enables Pairable.
It does not restart Bluetooth, clear bonds, or alter other GATT applications.

Start: sudo PYTHONPATH=... /usr/bin/python3 ble_hid_emulator.py
Send:  sudo PYTHONPATH=... /usr/bin/python3 ble_hid_emulator.py --send-key 0x52 --hold-ms 1500
Stop the server with SIGTERM; its prior Pairable setting is restored.
"""

import argparse
import asyncio
import contextlib
import json
import os
from pathlib import Path
import signal

from dbus_next import BusType, DBusError, Variant
from dbus_next.aio import MessageBus
from dbus_next.constants import PropertyAccess
from dbus_next.service import ServiceInterface, dbus_property, method

ROOT = "/org/amity/hid"
CONTROL_SOCKET = Path(__file__).with_suffix(".sock")
READ = PropertyAccess.READ


def uuid(number):
    return f"0000{number:04x}-0000-1000-8000-00805f9b34fb"


# ID 1: keyboard modifiers + reserved + six key array. ID 2: 16-bit consumer.
REPORT_MAP = bytes.fromhex("""
05 01 09 06 a1 01 85 01 05 07 19 e0 29 e7 15 00 25 01
75 01 95 08 81 02 95 01 75 08 81 01
05 07 19 00 29 81 15 00 26 81 00 75 08 95 06 81 00 c0
05 0c 09 01 a1 01 85 02 19 00 2a ff 03 15 00 26 ff 03
75 10 95 01 81 00 c0
""")


class Service(ServiceInterface):
    def __init__(self, path, number):
        super().__init__("org.bluez.GattService1")
        self.path = path
        self.number = number

    @dbus_property(access=READ)
    def UUID(self) -> "s":
        return uuid(self.number)

    @dbus_property(access=READ)
    def Primary(self) -> "b":
        return True

    @dbus_property(access=READ)
    def Includes(self) -> "ao":
        return []


class Characteristic(ServiceInterface):
    def __init__(self, path, service, number, flags, value=b""):
        super().__init__("org.bluez.GattCharacteristic1")
        self.path, self.service, self.number = path, service, number
        self.flags, self.value, self.notifying = flags, value, False

    @dbus_property(access=READ)
    def UUID(self) -> "s":
        return uuid(self.number)

    @dbus_property(access=READ)
    def Service(self) -> "o":
        return self.service

    @dbus_property(access=READ)
    def Flags(self) -> "as":
        return self.flags

    @dbus_property(access=READ)
    def Value(self) -> "ay":
        return self.value

    @dbus_property(access=READ)
    def Notifying(self) -> "b":
        return self.notifying

    @method()
    def ReadValue(self, options: "a{sv}") -> "ay":
        offset = options.get("offset", Variant("q", 0)).value
        return self.value[offset:]

    @method()
    def WriteValue(self, value: "ay", options: "a{sv}"):
        if not any("write" in flag for flag in self.flags):
            raise DBusError("org.bluez.Error.NotPermitted", "read only")
        self.value = value
        print(json.dumps({"write": hex(self.number), "value": value.hex()}), flush=True)

    @method()
    def StartNotify(self):
        self.notifying = True
        self.emit_properties_changed({"Notifying": True})
        print(json.dumps({"subscribed": hex(self.number), "path": self.path}), flush=True)

    @method()
    def StopNotify(self):
        self.notifying = False
        self.emit_properties_changed({"Notifying": False})
        print(json.dumps({"unsubscribed": self.path}), flush=True)

    def send(self, value):
        if not self.notifying:
            raise DBusError("org.amity.Error.NotConnected", "DUT has not subscribed")
        self.value = value
        self.emit_properties_changed({"Value": value})
        print(json.dumps({"notification": self.path, "value": value.hex()}), flush=True)


class ReportReference(ServiceInterface):
    def __init__(self, path, characteristic, report_id):
        super().__init__("org.bluez.GattDescriptor1")
        self.path, self.characteristic, self.report_id = path, characteristic, report_id

    @dbus_property(access=READ)
    def UUID(self) -> "s":
        return uuid(0x2908)

    @dbus_property(access=READ)
    def Characteristic(self) -> "o":
        return self.characteristic

    @dbus_property(access=READ)
    def Flags(self) -> "as":
        return ["read", "encrypt-read"]

    @method()
    def ReadValue(self, options: "a{sv}") -> "ay":
        return bytes([self.report_id, 1])


class Advertisement(ServiceInterface):
    def __init__(self):
        super().__init__("org.bluez.LEAdvertisement1")

    @dbus_property(access=READ)
    def Type(self) -> "s":
        return "peripheral"

    @dbus_property(access=READ)
    def ServiceUUIDs(self) -> "as":
        return [uuid(0x1812), uuid(0x180f)]

    @dbus_property(access=READ)
    def LocalName(self) -> "s":
        return "AmityHID"

    @dbus_property(access=READ)
    def Appearance(self) -> "q":
        return 0x03c1

    @dbus_property(access=READ)
    def Discoverable(self) -> "b":
        return True

    @method()
    def Release(self):
        print("Advertisement released", flush=True)


class Agent(ServiceInterface):
    def __init__(self):
        super().__init__("org.bluez.Agent1")

    @method()
    def RequestAuthorization(self, device: "o"):
        print(json.dumps({"authorized": device}), flush=True)

    @method()
    def AuthorizeService(self, device: "o", service: "s"):
        print(json.dumps({"authorized_service": service, "device": device}), flush=True)

    @method()
    def Release(self):
        pass

    @method()
    def Cancel(self):
        pass


class Control(ServiceInterface):
    def __init__(self, keyboard, consumer):
        super().__init__("org.amity.HIDTest1")
        self.keyboard, self.consumer = keyboard, consumer

    async def Key(self, usage, hold_ms):
        if not 0 <= usage <= 0x81 or not 0 <= hold_ms <= 30000:
            raise DBusError("org.amity.Error.InvalidArgument", "key or duration out of range")
        self.keyboard.send(bytes([0, 0, usage, 0, 0, 0, 0, 0]))
        await asyncio.sleep(hold_ms / 1000)
        self.keyboard.send(bytes(8))

    async def Consumer(self, usage, hold_ms):
        if not 0 <= usage <= 0x3ff or not 0 <= hold_ms <= 30000:
            raise DBusError("org.amity.Error.InvalidArgument", "key or duration out of range")
        self.consumer.send(usage.to_bytes(2, "little"))
        await asyncio.sleep(hold_ms / 1000)
        self.consumer.send(bytes(2))

    def Status(self):
        return {"keyboard_subscribed": self.keyboard.notifying,
                "consumer_subscribed": self.consumer.notifying}


async def main(args):
    if args.send_key is not None or args.send_consumer is not None or args.status or args.battery_level is not None or args.charging is not None:
        reader, writer = await asyncio.open_unix_connection(str(CONTROL_SOCKET))
        writer.write((json.dumps({"key": args.send_key, "consumer": args.send_consumer,
                                 "hold_ms": args.hold_ms, "battery_level": args.battery_level, "charging": args.charging}) + "\n").encode())
        await writer.drain()
        response = json.loads(await reader.readline())
        writer.close()
        await writer.wait_closed()
        if "error" in response:
            raise RuntimeError(response["error"])
        print(json.dumps(response))
        return

    bus = await MessageBus(bus_type=BusType.SYSTEM).connect()

    hid, battery = ROOT + "/service0", ROOT + "/service1"
    keyboard = Characteristic(hid + "/char2", hid, 0x2a4d,
                              ["read", "notify", "encrypt-read"], bytes(8))
    consumer = Characteristic(hid + "/char3", hid, 0x2a4d,
                              ["read", "notify", "encrypt-read"], bytes(2))
    battery_level = Characteristic(battery + "/char0", battery, 0x2a19, ["read", "notify"], bytes([87]))
    battery_status = Characteristic(battery + "/char1", battery, 0x2bed,
                                    ["read", "indicate" if args.battery_indicate else "notify"], bytes([2,0x41,0,87]))
    objects = [
        Service(hid, 0x1812),
        Characteristic(hid + "/char0", hid, 0x2a4a, ["read"], bytes([0x11, 0x01, 0, 2])),
        Characteristic(hid + "/char1", hid, 0x2a4b, ["read", "encrypt-read"], REPORT_MAP),
        keyboard, consumer,
        Characteristic(hid + "/char4", hid, 0x2a4e, ["read", "write-without-response"], b"\x01"),
        Characteristic(hid + "/char5", hid, 0x2a4c, ["write-without-response"], b"\x00"),
        ReportReference(keyboard.path + "/desc0", keyboard.path, 1),
        ReportReference(consumer.path + "/desc0", consumer.path, 2),
        Service(battery, 0x180f),
        battery_level, battery_status,
    ]
    for obj in objects:
        bus.export(obj.path, obj)
    control = Control(keyboard, consumer)
    bus.export(ROOT, control)
    bus.export(ROOT + "/agent", Agent())
    bus.export(ROOT + "/advertisement", Advertisement())
    # A root-only project-local socket avoids changing system D-Bus policy just
    # to inject test keys. BlueZ still uses the normal system-bus GATT API.
    async def on_control(reader, writer):
        try:
            request = json.loads(await reader.readline())
            if request.get("key") is not None:
                await control.Key(int(request["key"]), int(request["hold_ms"]))
            elif request.get("consumer") is not None:
                await control.Consumer(int(request["consumer"]), int(request["hold_ms"]))
            if request.get("battery_level") is not None or request.get("charging") is not None:
                level = request.get("battery_level")
                level = battery_level.value[0] if level is None else int(level)
                if not 0 <= level <= 100: raise ValueError("Battery must be 0..100")
                charging = request.get("charging")
                power = battery_status.value[1] if charging is None else {"yes":0x23,"no":0x41,"unknown":1}[charging]
                battery_level.send(bytes([level]))
                battery_status.send(bytes([2,power,0,level]))
            response = {**control.Status(), "battery_subscribed":battery_level.notifying,
                        "charging_subscribed":battery_status.notifying}
        except Exception as error:
            response = {"error": str(error)}
        writer.write((json.dumps(response) + "\n").encode())
        await writer.drain()
        writer.close()

    server = await asyncio.start_unix_server(on_control, str(CONTROL_SOCKET))
    os.chmod(CONTROL_SOCKET, 0o600)

    adapter_path = "/org/bluez/" + args.adapter
    adapter = bus.get_proxy_object("org.bluez", adapter_path, await bus.introspect("org.bluez", adapter_path))
    props = adapter.get_interface("org.freedesktop.DBus.Properties")
    gatt = adapter.get_interface("org.bluez.GattManager1")
    advertising = adapter.get_interface("org.bluez.LEAdvertisingManager1")
    bluez = bus.get_proxy_object("org.bluez", "/org/bluez", await bus.introspect("org.bluez", "/org/bluez"))
    agents = bluez.get_interface("org.bluez.AgentManager1")
    previous_pairable = await props.call_get("org.bluez.Adapter1", "Pairable")
    registered = []
    stop = asyncio.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        asyncio.get_running_loop().add_signal_handler(sig, stop.set)
    try:
        await agents.call_register_agent(ROOT + "/agent", "NoInputNoOutput")
        registered.append("agent")
        await agents.call_request_default_agent(ROOT + "/agent")
        await props.call_set("org.bluez.Adapter1", "Pairable", Variant("b", True))
        await gatt.call_register_application(ROOT, {})
        registered.append("gatt")
        await advertising.call_register_advertisement(ROOT + "/advertisement", {})
        registered.append("advertisement")
        print(json.dumps({"ready": True, "adapter": adapter_path, "name": "AmityHID"}), flush=True)
        await stop.wait()
    finally:
        for kind in reversed(registered):
            with contextlib.suppress(Exception):
                if kind == "advertisement":
                    await advertising.call_unregister_advertisement(ROOT + "/advertisement")
                elif kind == "gatt":
                    await gatt.call_unregister_application(ROOT)
                else:
                    await agents.call_unregister_agent(ROOT + "/agent")
        with contextlib.suppress(Exception):
            await props.call_set("org.bluez.Adapter1", "Pairable", previous_pairable)
        server.close()
        await server.wait_closed()
        with contextlib.suppress(OSError):
            CONTROL_SOCKET.unlink()
        bus.disconnect()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adapter", default="hci0")
    parser.add_argument("--send-key", type=lambda value: int(value, 0))
    parser.add_argument("--send-consumer", type=lambda value: int(value, 0))
    parser.add_argument("--hold-ms", type=int, default=150)
    parser.add_argument("--status", action="store_true")
    parser.add_argument("--battery-level", type=int, help="Notify a new percentage from the running fixture")
    parser.add_argument("--charging", choices=("yes","no","unknown"), help="Notify a Battery Level Status change")
    parser.add_argument("--battery-indicate", action="store_true", help="Use indications for Battery Level Status")
    asyncio.run(main(parser.parse_args()))
