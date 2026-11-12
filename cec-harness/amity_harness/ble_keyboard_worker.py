"""Pi-only BLE HID peripheral; authenticated DUT administration stays on the host.

The optional dbus-next dependency is loaded only when opening the physical
adapter. Run through the harness client, using a dedicated temporary GATT
application and advertisement on the existing BlueZ daemon.
"""

import argparse
import asyncio
import fcntl
import json
import os
from pathlib import Path
import re
import signal
import sys
import tempfile
import time


PROTOCOL_VERSION = 1
NAME = "Amity Test Keyboard"
ADVERTISING_INTERVAL_MS = (20, 30)
def _keyboard_slots(value):
    if type(value) is not int or value not in (6, 10):
        raise ValueError("Choose six or ten keyboard slots")
    return value


def report_map(keyboard_slots=6):
    slots = _keyboard_slots(keyboard_slots)
    return bytes.fromhex(
        "05 01 09 06 a1 01 85 01 "
        "05 07 19 e0 29 e7 15 00 25 01 75 01 95 08 81 02 "
        "95 01 75 08 81 01 "
        f"95 {slots:02x} 75 08 15 00 26 e7 00 19 00 29 e7 81 00 c0 "
        "05 0c 09 01 a1 01 85 02 15 00 26 ff ff 19 00 2a ff ff "
        "75 10 95 01 81 00 c0"
    )


REPORT_MAP = report_map()


def encode_reports(keyboard, consumer, keyboard_slots=6):
    """Build independent keyboard and consumer reports, without report-ID bytes."""
    _keyboard_slots(keyboard_slots)
    if (not isinstance(keyboard, list) or len(keyboard) > keyboard_slots + 8
            or any(type(key) is not int or not 4 <= key <= 0xe7 for key in keyboard)
            or len(set(keyboard)) != len(keyboard)):
        raise ValueError("Keyboard usages must be unique integers from 4 through 231")
    keys = sorted(key for key in keyboard if key < 0xe0)
    if len(keys) > keyboard_slots:
        raise ValueError("A keyboard report exceeds its ordinary-key capacity")
    if type(consumer) is not int or not 0 <= consumer <= 0xffff:
        raise ValueError("Consumer usage must be an integer from zero through 65535")
    modifiers = sum(1 << (key - 0xe0) for key in keyboard if key >= 0xe0)
    return {1: bytes((modifiers, 0, *keys, *([0] * (keyboard_slots - len(keys))))),
            2: consumer.to_bytes(2, "little")}


def _keys(value, names):
    if not isinstance(value, dict) or set(value) != set(names):
        raise ValueError("Unexpected BLE keyboard RPC fields")


def _plain(value):
    if hasattr(value, "value"):
        return _plain(value.value)
    if isinstance(value, dict):
        return {key: _plain(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_plain(item) for item in value]
    return value


class KeyboardWorker:
    """Strict RPC state and report encoding, independently testable without D-Bus."""

    def __init__(self, backend):
        self.backend = backend
        self.keyboard_slots = _keyboard_slots(getattr(backend, "keyboard_slots", 6))
        self.opened = self.closed = False
        self.reports = encode_reports([], 0, self.keyboard_slots)

    async def dispatch(self, operation, arguments):
        if operation == "open":
            _keys(arguments, ("protocol_version",))
            if type(arguments["protocol_version"]) is not int or arguments["protocol_version"] != PROTOCOL_VERSION:
                raise ValueError("Unsupported BLE keyboard RPC version")
            if self.opened or self.closed:
                raise RuntimeError("The BLE keyboard worker cannot be reopened")
            result = await self.backend.open()
            self.opened = True
            return {"protocol_version": PROTOCOL_VERSION, "keyboard_slots": self.keyboard_slots, **result}
        if operation == "close":
            _keys(arguments, ())
            await self.close()
            return {"closed": True}
        if not self.opened or self.closed:
            raise RuntimeError("The BLE keyboard has not been opened")
        if operation == "status":
            _keys(arguments, ())
            return await self.backend.status()
        if operation == "disconnect":
            _keys(arguments, ())
            await self.backend.disconnect()
            disconnected = time.monotonic_ns()
            status = await self.backend.status()
            if status.get("connected") is not False:
                raise RuntimeError("The keyboard link did not disconnect")
            # BlueZ can take seconds to finish its asynchronous Disconnect.
            # Timestamp completed link teardown before the separate status
            # read; its latency must not extend the physical CEC budget.
            return {**status, "disconnected_monotonic_ns": disconnected}
        if operation == "forget_peer":
            _keys(arguments, ())
            await self.backend.forget_peer()
            self.reports = encode_reports([], 0, self.keyboard_slots)
            return {"cleared": True}
        if operation != "report":
            raise ValueError("Unsupported BLE keyboard RPC operation")
        _keys(arguments, ("keyboard", "consumer"))
        reports = encode_reports(arguments["keyboard"], arguments["consumer"], self.keyboard_slots)
        status = await self.backend.status()
        if status.get("ready") is not True:
            raise RuntimeError("The bonded keyboard and consumer reports are not subscribed")
        changed = [number for number in (1, 2) if reports[number] != self.reports[number]]
        timestamp = time.monotonic_ns()
        for number in changed:
            await self.backend.publish(number, reports[number])
            self.reports[number] = reports[number]
        return {"timestamp_monotonic_ns": timestamp, "keyboard_report": reports[1].hex(),
                "consumer_report": reports[2].hex(), "changed": changed}

    async def close(self):
        if self.closed:
            return
        errors = []
        try:
            released = encode_reports([], 0, self.keyboard_slots)
            if self.opened:
                try:
                    status = await self.backend.status()
                except Exception as error:
                    errors.append(error)
                    status = {}
                if status.get("connected") is True and status.get("bonded") is True:
                    for number in (1, 2):
                        name = "keyboard" if number == 1 else "consumer"
                        if (self.reports[number] != released[number]
                                and status.get("notifying", {}).get(name) is True):
                            try:
                                await self.backend.publish(number, released[number])
                                self.reports[number] = released[number]
                            except Exception as error:
                                errors.append(error)
        finally:
            try:
                await self.backend.close()
            except Exception as error:
                errors.append(error)
            finally:
                self.closed = True
        if errors:
            raise RuntimeError("BLE keyboard cleanup failed: " + ", ".join(type(error).__name__ for error in errors))


class BlueZKeyboard:
    """One temporary peripheral using BlueZ's existing system D-Bus service."""

    def __init__(self, adapter="hci0", name=NAME, *, keyboard_slots=6):
        if not isinstance(adapter, str) or not re.fullmatch(r"hci[0-9]+", adapter):
            raise ValueError("Choose a BlueZ hci adapter")
        if not isinstance(name, str) or not 1 <= len(name.encode("utf-8")) <= 26 or any(ord(c) < 32 for c in name):
            raise ValueError("The BLE keyboard name must contain 1–26 printable UTF-8 bytes")
        self.adapter = "/org/bluez/" + adapter
        self.name = name
        self.keyboard_slots = _keyboard_slots(keyboard_slots)
        self.path = "/org/amity/keyboard/worker" + str(os.getpid())
        self.bus = self._lock = None
        self._application = self._advertising = self._agent = self._pairable_changed = False
        self._exports, self.characteristics = [], {}
        self.peer = None
        self._before_devices = {}
        self._before_connected = set()
        self._closed = False

    async def _call(self, path, interface, member, signature="", body=None):
        from dbus_next import Message, MessageType
        response = await asyncio.wait_for(self.bus.call(Message(
            destination="org.bluez", path=path, interface=interface, member=member,
            signature=signature, body=[] if body is None else body)), 5)
        if response.message_type == MessageType.ERROR:
            raise RuntimeError("BlueZ rejected " + member + " (" + response.error_name + ")")
        return response.body

    async def _objects(self):
        return _plain((await self._call("/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects"))[0])

    async def _set_pairable(self, value):
        from dbus_next import Variant
        await self._call(self.adapter, "org.freedesktop.DBus.Properties", "Set", "ssv",
                         ["org.bluez.Adapter1", "Pairable", Variant("b", value)])

    async def claim_peer(self, device):
        if not isinstance(device, str) or not device.startswith(self.adapter + "/dev_"):
            raise RuntimeError("The keyboard peer belongs to another adapter")
        if self.peer is not None and self.peer != device:
            raise RuntimeError("The keyboard already has a different peer")
        if device in self._before_connected:
            raise RuntimeError("An existing active Bluetooth connection is not fixture-owned")
        previous = self._before_devices.get(device, {})
        if previous.get("Bonded") is True or previous.get("Paired") is True:
            raise RuntimeError("An existing Bluetooth bond is not fixture-owned")
        objects = await self._objects()
        peer = objects.get(device, {}).get("org.bluez.Device1", {})
        if peer.get("Adapter") != self.adapter or peer.get("Connected") is not True:
            raise RuntimeError("The keyboard peer is not connected to this adapter")
        self.peer = device

    async def observe_device(self, options):
        device = _plain(options).get("device")
        if device is not None:
            await self.claim_peer(device)

    async def open(self):
        if self._closed or self.bus is not None:
            raise RuntimeError("The BLE peripheral adapter cannot be reopened")
        from dbus_next.aio import MessageBus
        from dbus_next import BusType
        lock = Path(tempfile.gettempdir()) / ("amity-ble-" + self.adapter.rsplit("/", 1)[-1] + ".lock")
        self._lock = os.open(lock, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        fcntl.flock(self._lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            self.bus = await asyncio.wait_for(MessageBus(bus_type=BusType.SYSTEM).connect(), 5)
            objects = await self._objects()
            interfaces = objects.get(self.adapter, {})
            adapter = interfaces.get("org.bluez.Adapter1", {})
            if adapter.get("Powered") is not True:
                raise RuntimeError("The Bluetooth adapter must already be powered")
            if ("org.bluez.GattManager1" not in interfaces
                    or "org.bluez.LEAdvertisingManager1" not in interfaces):
                raise RuntimeError("The adapter does not support a BLE GATT peripheral")
            active = interfaces["org.bluez.LEAdvertisingManager1"].get("ActiveInstances")
            if type(active) is not int or active != 0:
                raise RuntimeError("The adapter already has an active advertisement")
            if adapter.get("AddressType", "public") != "public":
                raise RuntimeError("The keyboard requires the adapter's stable public identity")
            self.address = adapter["Address"].lower()
            self._before_devices = {path: values["org.bluez.Device1"] for path, values in objects.items()
                                    if values.get("org.bluez.Device1", {}).get("Adapter") == self.adapter}
            self._before_connected = {path for path, values in self._before_devices.items()
                                      if values.get("Connected") is True}
            application, advertisement, agent = _interfaces(self)
            for path, interface in self._exports:
                self.bus.export(path, interface)
            await self._call("/org/bluez", "org.bluez.AgentManager1", "RegisterAgent", "os",
                             [agent, "NoInputNoOutput"])
            self._agent = True
            await self._call("/org/bluez", "org.bluez.AgentManager1", "RequestDefaultAgent", "o", [agent])
            if adapter.get("Pairable") is not True:
                self._pairable_changed = True
                await self._set_pairable(True)
            await self._call(self.adapter, "org.bluez.GattManager1", "RegisterApplication", "oa{sv}",
                             [application, {}])
            self._application = True
            await self._call(self.adapter, "org.bluez.LEAdvertisingManager1", "RegisterAdvertisement", "oa{sv}",
                             [advertisement, {}])
            self._advertising = True
            return {"address": self.address, "type": 0, "name": self.name}
        except BaseException as error:
            try:
                await self.close()
            except Exception as cleanup_error:
                error.add_note("BLE keyboard setup cleanup also failed: " + type(cleanup_error).__name__)
            raise

    async def status(self):
        objects = await self._objects()
        if self.peer is None:
            candidates = []
            for path, interfaces in objects.items():
                device = interfaces.get("org.bluez.Device1", {})
                previous = self._before_devices.get(path, {})
                if (device.get("Adapter") == self.adapter and device.get("Connected") is True
                        and path not in self._before_connected
                        and previous.get("Bonded") is not True and previous.get("Paired") is not True):
                    candidates.append(path)
            if len(candidates) > 1:
                raise RuntimeError("Multiple new Bluetooth connections cannot identify the keyboard peer")
            if candidates:
                # Just Works can complete before any Agent or GATT callback.
                # Keep that exact connection's identity for failure cleanup.
                await self.claim_peer(candidates[0])
        peer = objects.get(self.peer, {}).get("org.bluez.Device1", {}) if self.peer else {}
        connected = peer.get("Connected") is True
        bonded = peer.get("Bonded", peer.get("Paired", False)) is True
        previous = self._before_devices.get(self.peer, {})
        owned = (self.peer is not None and peer.get("Adapter") == self.adapter
                 and self.peer not in self._before_connected
                 and previous.get("Bonded") is not True and previous.get("Paired") is not True)
        notifying = {name: self.characteristics.get(number) is not None
                     and self.characteristics[number].notifying
                     for name, number in (("keyboard", 1), ("consumer", 2))}
        return {"connected": connected, "bonded": bonded,
                "peer_owned": owned,
                "ready": owned and connected and bonded and all(notifying.values()),
                "advertisement_registered": self._advertising and self._application,
                "notifying": notifying, "peer_address": peer.get("Address")}

    async def publish(self, number, value):
        self.characteristics[number].value = bytes(value)
        self.characteristics[number].emit_properties_changed({"Value": bytes(value)})
        await asyncio.sleep(0)

    async def _stop_advertising(self):
        if self._advertising:
            await self._call(self.adapter, "org.bluez.LEAdvertisingManager1", "UnregisterAdvertisement",
                             "o", [self.path + "/advertisement"])
            self._advertising = False

    async def disconnect(self):
        await self._stop_advertising()
        if self.peer:
            objects = await self._objects()
            if objects.get(self.peer, {}).get("org.bluez.Device1", {}).get("Connected") is True:
                await self._call(self.peer, "org.bluez.Device1", "Disconnect")

    async def forget_peer(self):
        """Forget only this fixture's disconnected peer, retaining all registrations."""
        if not self._advertising or not self._application or self.peer is None:
            raise RuntimeError("An advertised keyboard with an owned peer is required")
        previous = self._before_devices.get(self.peer, {})
        if (self.peer in self._before_connected or previous.get("Bonded") is True
                or previous.get("Paired") is True):
            raise RuntimeError("A preexisting Bluetooth peer cannot be removed")
        objects = await self._objects()
        peer = objects.get(self.peer, {}).get("org.bluez.Device1")
        if peer is not None:
            if peer.get("Adapter") != self.adapter or peer.get("Connected") is not False:
                raise RuntimeError("The owned keyboard peer must be disconnected before removal")
            await self._call(self.adapter, "org.bluez.Adapter1", "RemoveDevice", "o", [self.peer])
        self.peer = None
        released = encode_reports([], 0, self.keyboard_slots)
        for number, characteristic in self.characteristics.items():
            # BlueZ owns subscription lifetime, including retained CCC state.
            # Only its StartNotify/StopNotify callbacks change Notifying.
            characteristic.value = released[number]

    async def close(self):
        if self._closed:
            return
        errors = []

        async def attempt(action):
            try:
                await action()
            except Exception as error:
                errors.append(error)

        try:
            if self.bus is not None:
                await attempt(self.disconnect)
                if self.peer:
                    previous = self._before_devices.get(self.peer, {})
                    if previous.get("Bonded") is not True and previous.get("Paired") is not True:
                        await attempt(lambda: self._call(self.adapter, "org.bluez.Adapter1", "RemoveDevice", "o", [self.peer]))
                if self._application:
                    await attempt(lambda: self._call(self.adapter, "org.bluez.GattManager1", "UnregisterApplication", "o", [self.path]))
                if self._pairable_changed:
                    await attempt(lambda: self._set_pairable(False))
                if self._agent:
                    await attempt(lambda: self._call("/org/bluez", "org.bluez.AgentManager1", "UnregisterAgent", "o", [self.path + "/agent"]))
                for path, interface in reversed(self._exports):
                    self.bus.unexport(path, interface)
                self.bus.disconnect()
        finally:
            self._closed = True
            if self._lock is not None:
                os.close(self._lock)
                self._lock = None
        if errors:
            raise RuntimeError("BLE keyboard cleanup failed: " + ", ".join(type(error).__name__ for error in errors))


def _interfaces(owner):
    """Define the D-Bus service only on the Pi, after loading the optional dependency."""
    from dbus_next import DBusError, PropertyAccess, Variant
    from dbus_next.service import ServiceInterface, dbus_property, method

    uuid = lambda value: "0000" + value + "-0000-1000-8000-00805f9b34fb"
    service_path = owner.path + "/service0"
    managed = {}

    class Application(ServiceInterface):
        def __init__(self):
            super().__init__("org.freedesktop.DBus.ObjectManager")

        @method()
        def GetManagedObjects(self) -> 'a{oa{sa{sv}}}':
            return managed

    class Service(ServiceInterface):
        def __init__(self):
            super().__init__("org.bluez.GattService1")

        @dbus_property(access=PropertyAccess.READ)
        def UUID(self) -> 's':
            return uuid("1812")

        @dbus_property(access=PropertyAccess.READ)
        def Primary(self) -> 'b':
            return True

        @dbus_property(access=PropertyAccess.READ)
        def Includes(self) -> 'ao':
            return []

    class Characteristic(ServiceInterface):
        def __init__(self, code, value, flags, number=None):
            super().__init__("org.bluez.GattCharacteristic1")
            self.code, self.value, self.flags, self.number = code, bytes(value), flags, number
            self.notifying = False

        @dbus_property(access=PropertyAccess.READ)
        def UUID(self) -> 's':
            return uuid(self.code)

        @dbus_property(access=PropertyAccess.READ)
        def Service(self) -> 'o':
            return service_path

        @dbus_property(access=PropertyAccess.READ)
        def Flags(self) -> 'as':
            return self.flags

        @dbus_property(access=PropertyAccess.READ)
        def Value(self) -> 'ay':
            return self.value

        @dbus_property(access=PropertyAccess.READ)
        def Notifying(self) -> 'b':
            return self.notifying

        @method()
        async def ReadValue(self, options: 'a{sv}') -> 'ay':
            await owner.observe_device(options)
            offset = _plain(options).get("offset", 0)
            if not 0 <= offset <= len(self.value):
                raise DBusError("org.bluez.Error.InvalidOffset", "Invalid report offset")
            return self.value[offset:]

        @method()
        async def WriteValue(self, value: 'ay', options: 'a{sv}'):
            await owner.observe_device(options)
            if self.code not in ("2a4c", "2a4e") or len(value) != 1 or value[0] not in (0, 1):
                raise DBusError("org.bluez.Error.NotSupported", "Unsupported keyboard write")
            self.value = bytes(value)

        @method()
        def StartNotify(self):
            if self.number is None:
                raise DBusError("org.bluez.Error.NotSupported", "Not an input report")
            self.notifying = True
            self.emit_properties_changed({"Notifying": True})

        @method()
        def StopNotify(self):
            self.notifying = False
            self.emit_properties_changed({"Notifying": False})

    class Reference(ServiceInterface):
        def __init__(self, characteristic, number):
            super().__init__("org.bluez.GattDescriptor1")
            self.characteristic, self.number = characteristic, number

        @dbus_property(access=PropertyAccess.READ)
        def UUID(self) -> 's':
            return uuid("2908")

        @dbus_property(access=PropertyAccess.READ)
        def Characteristic(self) -> 'o':
            return self.characteristic

        @dbus_property(access=PropertyAccess.READ)
        def Flags(self) -> 'as':
            return ["read", "encrypt-read"]

        @method()
        async def ReadValue(self, options: 'a{sv}') -> 'ay':
            await owner.observe_device(options)
            return bytes((self.number, 1))

    class Advertisement(ServiceInterface):
        def __init__(self):
            super().__init__("org.bluez.LEAdvertisement1")

        @dbus_property(access=PropertyAccess.READ)
        def Type(self) -> 's':
            return "peripheral"

        @dbus_property(access=PropertyAccess.READ)
        def Discoverable(self) -> 'b':
            return True

        @dbus_property(access=PropertyAccess.READ)
        def ServiceUUIDs(self) -> 'as':
            return [uuid("1812")]

        @dbus_property(access=PropertyAccess.READ)
        def LocalName(self) -> 's':
            return owner.name

        @dbus_property(access=PropertyAccess.READ)
        def Appearance(self) -> 'q':
            return 0x03c1

        # A HID keyboard awaiting a connection advertises at the HOGP
        # fast-connection rate, not the adapter's 1.28 s default. BlueZ applies
        # these only when bluetoothd runs with --experimental (see setup.py).
        @dbus_property(access=PropertyAccess.READ)
        def MinInterval(self) -> 'u':
            return ADVERTISING_INTERVAL_MS[0]

        @dbus_property(access=PropertyAccess.READ)
        def MaxInterval(self) -> 'u':
            return ADVERTISING_INTERVAL_MS[1]

        @method()
        def Release(self):
            owner._advertising = False

    class Agent(ServiceInterface):
        def __init__(self):
            super().__init__("org.bluez.Agent1")

        @method()
        def Release(self):
            owner._agent = False

        async def _authorize(self, device):
            try:
                if not owner._advertising:
                    raise RuntimeError("The fixture is not advertising")
                await owner.claim_peer(device)
            except RuntimeError:
                raise DBusError("org.bluez.Error.Rejected", "Not this keyboard's peer")

        @method()
        async def RequestAuthorization(self, device: 'o'):
            await self._authorize(device)

        @method()
        async def AuthorizeService(self, device: 'o', service: 's'):
            if service not in (uuid("1812"), uuid("1800"), uuid("180a"), uuid("180f")):
                raise DBusError("org.bluez.Error.Rejected", "Not a keyboard service")
            await self._authorize(device)

        @method()
        def RequestPinCode(self, device: 'o') -> 's':
            raise DBusError("org.bluez.Error.Rejected", "Only Just Works pairing is supported")

        @method()
        def RequestPasskey(self, device: 'o') -> 'u':
            raise DBusError("org.bluez.Error.Rejected", "Only Just Works pairing is supported")

        @method()
        def RequestConfirmation(self, device: 'o', passkey: 'u'):
            raise DBusError("org.bluez.Error.Rejected", "Only Just Works pairing is supported")

        @method()
        def Cancel(self):
            pass

    owner._exports.append((owner.path, Application()))
    owner._exports.append((service_path, Service()))
    managed[service_path] = {"org.bluez.GattService1": {
        "UUID": Variant("s", uuid("1812")), "Primary": Variant("b", True), "Includes": Variant("ao", [])}}
    definitions = (("2a4a", b"\x11\x01\x00\x02", ["read", "encrypt-read"], None),
                   ("2a4b", report_map(owner.keyboard_slots), ["read", "encrypt-read"], None),
                   ("2a4c", b"\x00", ["write-without-response", "encrypt-write"], None),
                   ("2a4e", b"\x01", ["read", "write-without-response", "encrypt-read", "encrypt-write"], None),
                   ("2a4d", bytes(2 + owner.keyboard_slots), ["read", "notify", "encrypt-read"], 1),
                   ("2a4d", bytes(2), ["read", "notify", "encrypt-read"], 2))
    for index, (code, value, flags, number) in enumerate(definitions):
        path = service_path + "/char" + str(index)
        characteristic = Characteristic(code, value, flags, number)
        owner._exports.append((path, characteristic))
        managed[path] = {"org.bluez.GattCharacteristic1": {
            "UUID": Variant("s", uuid(code)), "Service": Variant("o", service_path), "Flags": Variant("as", flags)}}
        if number is not None:
            owner.characteristics[number] = characteristic
            descriptor = path + "/reference"
            owner._exports.append((descriptor, Reference(path, number)))
            managed[descriptor] = {"org.bluez.GattDescriptor1": {
                "UUID": Variant("s", uuid("2908")), "Characteristic": Variant("o", path),
                "Flags": Variant("as", ["read", "encrypt-read"])}}
    advertisement, agent = owner.path + "/advertisement", owner.path + "/agent"
    owner._exports.extend(((advertisement, Advertisement()), (agent, Agent())))
    return owner.path, advertisement, agent


async def serve(reader, writer, backend, stop=None):
    worker = KeyboardWorker(backend)
    try:
        while stop is None or not stop.is_set():
            request_id = None
            operation = None
            try:
                read = asyncio.create_task(reader.readline())
                if stop is not None:
                    stopping = asyncio.create_task(stop.wait())
                    done, _ = await asyncio.wait((read, stopping), return_when=asyncio.FIRST_COMPLETED)
                    if stopping in done:
                        read.cancel()
                        await asyncio.gather(read, return_exceptions=True)
                        break
                    stopping.cancel()
                    await asyncio.gather(stopping, return_exceptions=True)
                line = await read
                if not line:
                    break
                if len(line) > 4096:
                    raise ValueError("BLE keyboard RPC line is too long")
                request = json.loads(line)
                _keys(request, ("id", "operation", "arguments"))
                request_id = request["id"]
                if type(request_id) is not int or not 1 <= request_id < 2**53:
                    raise ValueError("Invalid BLE keyboard request identity")
                operation = request["operation"]
                result = await worker.dispatch(operation, request["arguments"])
                response = {"id": request_id, "ok": True, "result": result}
            except Exception as error:
                response = {"id": request_id, "ok": False, "error": type(error).__name__ + ": " + str(error)}
                writer.write(json.dumps(response, separators=(",", ":")) + "\n")
                writer.flush()
                break
            writer.write(json.dumps(response, separators=(",", ":")) + "\n")
            writer.flush()
            if operation == "close":
                break
    finally:
        await worker.close()


async def _main(args):
    reader = asyncio.StreamReader(limit=4096)
    await asyncio.get_running_loop().connect_read_pipe(lambda: asyncio.StreamReaderProtocol(reader), sys.stdin)
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for signum in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        loop.add_signal_handler(signum, stop.set)
    await serve(reader, sys.stdout, BlueZKeyboard(args.adapter, args.name, keyboard_slots=args.keyboard_slots), stop)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Temporary BlueZ BLE keyboard stdio worker")
    parser.add_argument("--adapter", default="hci0")
    parser.add_argument("--name", default=NAME)
    parser.add_argument("--keyboard-slots", type=int, choices=(6, 10), default=6)
    args = parser.parse_args(argv)
    asyncio.run(_main(args))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
