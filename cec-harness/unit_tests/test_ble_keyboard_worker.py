"""BLE report/RPC ownership and cleanup without a Bluetooth adapter or D-Bus."""

from io import StringIO
import json
from pathlib import Path
import sys
import tempfile
from types import ModuleType, SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness.ble_keyboard_worker import BlueZKeyboard, KeyboardWorker, _interfaces, encode_reports, report_map, REPORT_MAP, serve


class FakeBackend:
    def __init__(self):
        self.events = []
        self.ready = True
        self.fail_publish = False
        self.fail_close = False

    async def open(self):
        self.events.append(("open",))
        return {"address": "11:22:33:44:55:66", "type": 0, "name": "Amity Test Keyboard"}

    async def status(self):
        return {"ready": self.ready, "connected": self.ready, "bonded": self.ready,
                "notifying": {"keyboard": self.ready, "consumer": self.ready},
                "peer_address": "22:33:44:55:66:77" if self.ready else None}

    async def publish(self, number, value):
        if self.fail_publish:
            raise RuntimeError("Notification failed")
        self.events.append(("publish", number, value.hex()))

    async def disconnect(self):
        self.events.append(("disconnect",))
        self.ready = False

    async def forget_peer(self):
        if self.ready:
            raise RuntimeError("Peer remains connected")
        self.events.append(("forget_peer",))

    async def close(self):
        self.events.append(("close",))
        if self.fail_close:
            raise RuntimeError("Cleanup failed")


class FakeReader:
    def __init__(self, requests):
        self.lines = [json.dumps(request).encode() + b"\n" for request in requests]

    async def readline(self):
        return self.lines.pop(0) if self.lines else b""


class KeyboardReportTests(unittest.TestCase):
    def test_independent_keyboard_consumer_modifiers_and_little_endian_payloads(self):
        reports = encode_reports([0x4f, 0xe1, 0xe0], 0x00e9)
        self.assertEqual(reports, {1: bytes.fromhex("03 00 4f 00 00 00 00 00"),
                                   2: bytes.fromhex("e9 00")})
        self.assertEqual(encode_reports([0x52, 0x4f], 0), encode_reports([0x4f, 0x52], 0))
        self.assertEqual(encode_reports([], 0), {1: bytes(8), 2: bytes(2)})

    def test_optional_ten_slot_map_encodes_one_complete_keyboard_report(self):
        keys = [0x28, 0x29, 0x4a, 0x4f, 0x50, 0x51, 0x52, 0x7f, 0x80, 0x81]
        self.assertEqual(encode_reports(keys, 0, 10), {1: bytes([0, 0, *keys]), 2: bytes(2)})
        self.assertEqual(encode_reports([], 0, 10), {1: bytes(12), 2: bytes(2)})
        self.assertEqual(report_map(), REPORT_MAP)
        self.assertIn(bytes.fromhex("95 0a 75 08 15 00"), report_map(10))
        self.assertEqual(report_map(10).replace(bytes.fromhex("95 0a 75 08 15 00"),
                                              bytes.fromhex("95 06 75 08 15 00")), REPORT_MAP)
        with self.assertRaises(ValueError): encode_reports(keys, 0)
        for slots in (True, 0, 7, 16):
            with self.subTest(slots=slots), self.assertRaises(ValueError): report_map(slots)

    def test_invalid_rollover_duplicate_or_malformed_reports_are_rejected(self):
        for keyboard, consumer in (([1], 0), ([True], 0), ([0x4f, 0x4f], 0),
                                   (list(range(4, 11)), 0), ([0xe8], 0), ((), 0),
                                   ([], True), ([], -1), ([], 0x10000)):
            with self.subTest(keyboard=keyboard, consumer=consumer), self.assertRaises(ValueError):
                encode_reports(keyboard, consumer)


class KeyboardWorkerTests(unittest.IsolatedAsyncioTestCase):
    async def opened(self):
        backend = FakeBackend()
        worker = KeyboardWorker(backend)
        result = await worker.dispatch("open", {"protocol_version": 1})
        self.assertEqual(result["protocol_version"], 1)
        self.assertEqual(result["name"], "Amity Test Keyboard")
        return worker, backend

    async def test_overlapping_reports_release_only_the_changed_report(self):
        worker, backend = await self.opened()
        for keys, consumer in (([], 0xe9), ([0x4f], 0xe9), ([], 0xe9), ([], 0)):
            result = await worker.dispatch("report", {"keyboard": keys, "consumer": consumer})
            self.assertGreater(result["timestamp_monotonic_ns"], 0)
        self.assertEqual(backend.events[1:], [
            ("publish", 2, "e900"), ("publish", 1, "00004f0000000000"),
            ("publish", 1, "0000000000000000"), ("publish", 2, "0000")])
        unchanged = await worker.dispatch("report", {"keyboard": [], "consumer": 0})
        self.assertEqual(unchanged["changed"], [])
        await worker.close()

    async def test_ten_key_report_publishes_one_notification_then_releases_its_full_width(self):
        backend = FakeBackend()
        backend.keyboard_slots = 10
        worker = KeyboardWorker(backend)
        opened = await worker.dispatch("open", {"protocol_version": 1})
        self.assertEqual(opened["keyboard_slots"], 10)
        keys = [0x28, 0x29, 0x4a, 0x4f, 0x50, 0x51, 0x52, 0x7f, 0x80, 0x81]
        result = await worker.dispatch("report", {"keyboard": keys, "consumer": 0})
        self.assertEqual(result["changed"], [1])
        self.assertEqual(backend.events, [("open",), ("publish", 1, bytes([0, 0, *keys]).hex())])
        await worker.close()
        self.assertEqual(backend.events[-2:], [("publish", 1, bytes(12).hex()), ("close",)])

    async def test_reports_require_bond_and_both_subscriptions(self):
        worker, backend = await self.opened()
        backend.ready = False
        with self.assertRaisesRegex(RuntimeError, "not subscribed"):
            await worker.dispatch("report", {"keyboard": [0x4f], "consumer": 0xe9})
        self.assertEqual(backend.events, [("open",)])
        self.assertEqual(worker.reports, encode_reports([], 0))
        await worker.close()

    async def test_invalid_or_failed_publish_does_not_claim_successful_state(self):
        worker, backend = await self.opened()
        with self.assertRaises(ValueError):
            await worker.dispatch("report", {"keyboard": [0x4f], "consumer": -1})
        backend.fail_publish = True
        with self.assertRaisesRegex(RuntimeError, "Notification failed"):
            await worker.dispatch("report", {"keyboard": [0x4f], "consumer": 0})
        self.assertEqual(worker.reports, encode_reports([], 0))
        backend.fail_publish = False
        await worker.close()

    async def test_close_releases_each_held_report_and_runs_once(self):
        worker, backend = await self.opened()
        await worker.dispatch("report", {"keyboard": [0x4f], "consumer": 0xe9})
        await worker.close()
        await worker.close()
        self.assertEqual(backend.events[-3:], [("publish", 1, "0000000000000000"),
                                              ("publish", 2, "0000"), ("close",)])
        with self.assertRaises(RuntimeError):
            await worker.dispatch("open", {"protocol_version": 1})

    async def test_disconnect_cuts_link_with_holds_still_down(self):
        worker, backend = await self.opened()
        await worker.dispatch("report", {"keyboard": [0x4f], "consumer": 0xe9})
        status = await worker.dispatch("disconnect", {})
        self.assertFalse(status["connected"])
        self.assertGreater(status["disconnected_monotonic_ns"], 0)
        self.assertEqual(backend.events[-1], ("disconnect",))
        self.assertNotEqual(worker.reports, encode_reports([], 0))
        await worker.close()
        self.assertEqual(backend.events[-2:], [("disconnect",), ("close",)])

    async def test_disconnect_requires_confirmed_link_loss(self):
        worker, backend = await self.opened()
        async def still_connected():
            backend.events.append(("disconnect",))
        backend.disconnect = still_connected
        with self.assertRaisesRegex(RuntimeError, "link did not disconnect"):
            await worker.dispatch("disconnect", {})
        await worker.close()

    async def test_disconnect_status_read_cannot_extend_the_cec_deadline(self):
        worker, backend = await self.opened()
        clock = [1_000_000_000]
        original_status = backend.status
        async def delayed_status():
            clock[0] += 2_000_000_000
            return await original_status()
        backend.status = delayed_status
        with patch("amity_harness.ble_keyboard_worker.time.monotonic_ns", side_effect=lambda: clock[0]):
            receipt = await worker.dispatch("disconnect", {})
        self.assertFalse(receipt["connected"])
        self.assertEqual(receipt["disconnected_monotonic_ns"], 1_000_000_000)
        # The normal one-second CEC deadline has already expired while the
        # read was pending; receiving its reply cannot create a new budget.
        self.assertLess(receipt["disconnected_monotonic_ns"] + 1_000_000_000, clock[0])
        await worker.close()

    async def test_forget_peer_resets_report_state_only_after_disconnected_backend_cleanup(self):
        worker, backend = await self.opened()
        await worker.dispatch("report", {"keyboard": [0x4f], "consumer": 0xe9})
        held = worker.reports.copy()
        with self.assertRaisesRegex(RuntimeError, "remains connected"):
            await worker.dispatch("forget_peer", {})
        self.assertEqual(worker.reports, held)
        backend.ready = False
        self.assertEqual(await worker.dispatch("forget_peer", {}), {"cleared": True})
        self.assertEqual(worker.reports, encode_reports([], 0))
        self.assertEqual(backend.events[-1], ("forget_peer",))
        self.assertNotIn(("close",), backend.events)
        self.assertNotIn(("disconnect",), backend.events)
        backend.ready = True
        fresh = await worker.dispatch("report", {"keyboard": [0x4f], "consumer": 0})
        self.assertEqual(fresh["changed"], [1])
        await worker.close()

    async def test_cleanup_releases_a_remaining_subscription_after_partial_unsubscribe(self):
        worker, backend = await self.opened()
        await worker.dispatch("report", {"keyboard": [0x4f], "consumer": 0xe9})

        async def status():
            return {"ready": False, "connected": True, "bonded": True,
                    "notifying": {"keyboard": True, "consumer": False}}

        backend.status = status
        await worker.close()
        self.assertEqual(backend.events[-2:], [("publish", 1, "0000000000000000"), ("close",)])

    async def test_cleanup_tries_other_report_and_backend_after_notification_failure(self):
        worker, backend = await self.opened()
        await worker.dispatch("report", {"keyboard": [0x4f], "consumer": 0xe9})
        original = backend.publish

        async def publish(number, value):
            if number == 1:
                raise RuntimeError("Keyboard notification failed")
            await original(number, value)

        backend.publish = publish
        with self.assertRaisesRegex(RuntimeError, "cleanup failed"):
            await worker.close()
        self.assertEqual(backend.events[-2:], [("publish", 2, "0000"), ("close",)])
        self.assertTrue(worker.closed)

    async def test_eof_and_invalid_rpc_always_close_without_stdout_noise(self):
        for bad in (None, {"id": 3, "operation": "report", "arguments": {"keyboard": [1], "consumer": 0}},
                    {"id": True, "operation": "status", "arguments": {}},
                    {"id": 3, "operation": "status", "arguments": {"extra": True}}):
            with self.subTest(bad=bad):
                backend, output = FakeBackend(), StringIO()
                requests = [{"id": 1, "operation": "open", "arguments": {"protocol_version": 1}},
                            {"id": 2, "operation": "report", "arguments": {"keyboard": [0x4f], "consumer": 0}}]
                if bad is not None:
                    requests.append(bad)
                await serve(FakeReader(requests), output, backend)
                responses = [json.loads(line) for line in output.getvalue().splitlines()]
                self.assertEqual(responses[0]["result"]["protocol_version"], 1)
                self.assertEqual(responses[-1]["ok"], bad is None)
                self.assertEqual(backend.events[-2:], [("publish", 1, "0000000000000000"), ("close",)])

    async def test_state_and_exact_rpc_version_are_required(self):
        worker = KeyboardWorker(FakeBackend())
        for operation, arguments in (("status", {}), ("open", {"protocol_version": True}),
                                     ("open", {"protocol_version": 2}), ("open", {})):
            with self.subTest(operation=operation, arguments=arguments), self.assertRaises((ValueError, RuntimeError)):
                await worker.dispatch(operation, arguments)
        await worker.close()


class BlueZOwnershipTests(unittest.IsolatedAsyncioTestCase):
    def fixture(self, *, previous_bond=False):
        owner = BlueZKeyboard()
        owner.peer = owner.adapter + "/dev_11_22_33_44_55_66"
        owner._before_devices = {owner.peer: {"Paired": previous_bond}}
        owner._application = owner._advertising = owner._agent = owner._pairable_changed = True
        events = []

        class Bus:
            def unexport(self, path, interface):
                events.append(("unexport", path))

            def disconnect(self):
                events.append(("bus_disconnect",))

        owner.bus = Bus()

        async def objects():
            return {owner.peer: {"org.bluez.Device1": {"Connected": True, "Adapter": owner.adapter,
                                                      "Address": "11:22:33:44:55:66", "Paired": True}}}

        async def call(path, interface, member, signature="", body=None):
            events.append((member, path, body))
            return []

        async def pairable(value):
            events.append(("pairable", value))

        owner._objects, owner._call, owner._set_pairable = objects, call, pairable
        return owner, events

    async def test_cleanup_removes_only_fixture_bond_and_restores_pairable_and_services(self):
        for previous in (False, True):
            with self.subTest(previous=previous):
                owner, events = self.fixture(previous_bond=previous)
                await owner.close()
                await owner.close()
                names = [event[0] for event in events]
                self.assertEqual(names.count("RemoveDevice"), 0 if previous else 1)
                self.assertEqual(names.count("Disconnect"), 1)
                self.assertLess(names.index("UnregisterAdvertisement"), names.index("Disconnect"))
                self.assertIn(("pairable", False), events)
                self.assertIn("UnregisterApplication", names)
                self.assertIn("UnregisterAgent", names)
                self.assertEqual(names[-1], "bus_disconnect")

    async def test_forget_disconnected_owned_peer_keeps_advertisement_and_gatt_registered(self):
        owner, events = self.fixture()
        peer = owner.peer
        async def objects():
            return {peer: {"org.bluez.Device1": {"Connected": False, "Adapter": owner.adapter,
                                                 "Bonded": True}}}
        owner._objects = objects
        owner.characteristics = {number: SimpleNamespace(notifying=True, value=b"held") for number in (1, 2)}
        await owner.forget_peer()
        self.assertIsNone(owner.peer)
        self.assertEqual(events, [("RemoveDevice", owner.adapter, [peer])])
        self.assertTrue(owner._advertising and owner._application and owner._agent)
        for number, characteristic in owner.characteristics.items():
            self.assertTrue(characteristic.notifying)
            self.assertEqual(characteristic.value, encode_reports([], 0)[number])

    async def test_peer_reuse_preserves_only_actual_bluez_notify_callbacks(self):
        class Interface:
            def __init__(self, name): self.name = name
            def emit_properties_changed(self, values): pass

        dbus, service = ModuleType("dbus_next"), ModuleType("dbus_next.service")
        dbus.DBusError = RuntimeError
        dbus.PropertyAccess = SimpleNamespace(READ=1)
        dbus.Variant = lambda signature, value: SimpleNamespace(value=value)
        service.ServiceInterface = Interface
        service.dbus_property = lambda **kwargs: property
        service.method = lambda **kwargs: lambda function: function

        for retained in (False, True):
            with self.subTest(retained=retained):
                owner, events = self.fixture()
                peer = owner.peer
                objects = {peer: {"org.bluez.Device1": {"Connected": False,
                           "Bonded": True, "Adapter": owner.adapter}}}
                async def current(): return objects
                owner._objects = current
                with patch.dict(sys.modules, {"dbus_next": dbus, "dbus_next.service": service}):
                    _interfaces(owner)
                if retained:
                    for characteristic in owner.characteristics.values(): characteristic.StartNotify()
                await owner.forget_peer()
                self.assertTrue(all(characteristic.notifying is retained
                                    for characteristic in owner.characteristics.values()))
                self.assertFalse((await owner.status())["ready"])
                objects[peer]["org.bluez.Device1"]["Connected"] = True
                self.assertIs((await owner.status())["ready"], retained)
                # A retained subscription needs no repeated StartNotify. A real
                # StopNotify still blocks readiness until new callbacks arrive.
                for characteristic in owner.characteristics.values(): characteristic.StopNotify()
                self.assertFalse((await owner.status())["ready"])
                for characteristic in owner.characteristics.values(): characteristic.StartNotify()
                self.assertTrue((await owner.status())["ready"])
                # Reconnection returns the advertisement to the connected interval.
                self.assertEqual([event[0] for event in events],
                                 ["RemoveDevice", "UnregisterAdvertisement", "RegisterAdvertisement"])

    def test_advertisement_requests_hid_fast_connection_interval(self):
        dbus, service = ModuleType("dbus_next"), ModuleType("dbus_next.service")
        dbus.DBusError = RuntimeError
        dbus.PropertyAccess = SimpleNamespace(READ=1)
        dbus.Variant = lambda signature, value: SimpleNamespace(value=value)
        service.ServiceInterface = type("Interface", (), {"__init__": lambda self, name: None})
        service.dbus_property = lambda **kwargs: property
        service.method = lambda **kwargs: lambda function: function
        owner, _ = self.fixture()
        owner._exports = []
        with patch.dict(sys.modules, {"dbus_next": dbus, "dbus_next.service": service}):
            _interfaces(owner)
        advertisement = dict(owner._exports)[owner.path + "/advertisement"]
        self.assertEqual((advertisement.MinInterval, advertisement.MaxInterval), (20, 30))
        owner._connected_advertising = True
        self.assertEqual((advertisement.MinInterval, advertisement.MaxInterval), (1280, 1280))

    async def test_advertising_interval_follows_the_keyboard_link(self):
        owner, events = self.fixture()
        connected = True
        async def objects():
            return {owner.peer: {"org.bluez.Device1": {"Connected": connected, "Adapter": owner.adapter,
                                                      "Paired": True}}}
        owner._objects = objects
        advertisement = ("RegisterAdvertisement", owner.adapter, [owner.path + "/advertisement", {}])
        for connected in (True, True, False, False, True):
            events.clear()
            changed = owner._connected_advertising != connected
            self.assertIs((await owner.status())["advertisement_registered"], True)
            self.assertIs(owner._connected_advertising, connected)
            self.assertEqual(events, [("UnregisterAdvertisement", owner.adapter, [owner.path + "/advertisement"]),
                                      advertisement] if changed else [])
        await owner.disconnect()
        events.clear()
        connected = False
        self.assertIs((await owner.status())["advertisement_registered"], False)
        self.assertEqual(events, [])

    async def test_forget_rejects_live_foreign_preexisting_or_unadvertised_peer(self):
        for changed in ("connected", "adapter", "bonded", "before_connected", "advertisement"):
            with self.subTest(changed=changed):
                owner, events = self.fixture(previous_bond=changed == "bonded")
                peer = owner.peer
                if changed == "before_connected": owner._before_connected.add(peer)
                if changed == "advertisement": owner._advertising = False
                async def objects():
                    return {peer: {"org.bluez.Device1": {"Connected": changed == "connected",
                        "Adapter": "/org/bluez/hci1" if changed == "adapter" else owner.adapter}}}
                owner._objects = objects
                with self.assertRaises(RuntimeError): await owner.forget_peer()
                self.assertEqual(owner.peer, peer)
                self.assertEqual(events, [])

    async def test_forget_failure_retains_exact_peer_for_final_cleanup(self):
        owner, events = self.fixture()
        peer = owner.peer
        async def objects():
            return {peer: {"org.bluez.Device1": {"Connected": False, "Adapter": owner.adapter}}}
        async def call(*args): raise RuntimeError("Remove failed")
        owner._objects, owner._call = objects, call
        with self.assertRaisesRegex(RuntimeError, "Remove failed"): await owner.forget_peer()
        self.assertEqual(owner.peer, peer)
        self.assertTrue(owner._advertising and owner._application)

    async def test_agent_claim_rejects_foreign_or_already_active_connections(self):
        owner, _ = self.fixture()
        with self.assertRaises(RuntimeError):
            await owner.claim_peer("/org/bluez/hci1/dev_11_22_33_44_55_66")
        with self.assertRaises(RuntimeError):
            await owner.claim_peer(owner.adapter + "/dev_22_33_44_55_66_77")
        owner._before_connected.add(owner.peer)
        with self.assertRaisesRegex(RuntimeError, "existing active"):
            await owner.claim_peer(owner.peer)
        owner._before_connected.clear()
        await owner.claim_peer(owner.peer)
        await owner.close()

    async def test_agent_and_gatt_claims_reject_preexisting_bonds(self):
        for field in ("Bonded", "Paired"):
            with self.subTest(field=field):
                owner, events = self.fixture()
                owner._before_devices[owner.peer] = {field: True}
                with self.assertRaisesRegex(RuntimeError, "existing Bluetooth bond"):
                    await owner.claim_peer(owner.peer)
                with self.assertRaisesRegex(RuntimeError, "existing Bluetooth bond"):
                    await owner.observe_device({"device": owner.peer})
                self.assertEqual(events, [])

    async def test_cleanup_attempts_remaining_restoration_after_one_failure(self):
        owner, events = self.fixture()
        normal = owner._call

        async def call(path, interface, member, signature="", body=None):
            if member == "RemoveDevice":
                raise RuntimeError("Injected removal failure")
            return await normal(path, interface, member, signature, body)

        owner._call = call
        with self.assertRaisesRegex(RuntimeError, "cleanup failed"):
            await owner.close()
        self.assertIn(("pairable", False), events)
        self.assertIn("UnregisterAgent", [event[0] for event in events])
        self.assertEqual(events[-1], ("bus_disconnect",))

    async def test_status_claims_an_early_bond_before_agent_or_gatt_and_cleans_it_after_disconnect(self):
        owner, events = self.fixture()
        device = owner.peer
        owner.peer = None
        objects = {device: {"org.bluez.Device1": {
            "Adapter": owner.adapter, "Connected": True, "Paired": True, "Bonded": True,
            "Address": "11:22:33:44:55:66"}}}

        async def current():
            return objects

        owner._objects = current
        status = await owner.status()
        self.assertEqual(owner.peer, device)
        self.assertTrue(status["connected"])
        self.assertTrue(status["bonded"])
        self.assertFalse(status["ready"])
        objects[device]["org.bluez.Device1"]["Connected"] = False
        await owner.close()
        self.assertEqual([event[2] for event in events if event[0] == "RemoveDevice"], [[device]])

    async def test_status_never_adopts_preexisting_bonds_or_active_connections(self):
        for previous in ({"Paired": True}, {"Bonded": True}, {"Paired": True, "Bonded": False},
                         {"Connected": True}):
            with self.subTest(previous=previous):
                owner, events = self.fixture()
                device = owner.peer
                owner.peer = None
                owner._before_devices = {device: previous}
                owner._before_connected = {device} if previous.get("Connected") else set()
                objects = {device: {"org.bluez.Device1": {
                    "Adapter": owner.adapter, "Connected": True, "Paired": True, "Bonded": True}}}

                async def current():
                    return objects

                owner._objects = current
                status = await owner.status()
                self.assertIsNone(owner.peer)
                self.assertFalse(status["connected"])
                await owner.close()
                self.assertNotIn("RemoveDevice", [event[0] for event in events])
                self.assertNotIn("Disconnect", [event[0] for event in events])

    async def test_status_rejects_ambiguous_new_connections_without_claiming_or_removing_either(self):
        owner, events = self.fixture()
        device = owner.peer
        owner.peer = None
        second = owner.adapter + "/dev_22_33_44_55_66_77"
        objects = {path: {"org.bluez.Device1": {"Adapter": owner.adapter, "Connected": True,
                                                 "Bonded": True}} for path in (device, second)}

        async def current():
            return objects

        owner._objects = current
        with self.assertRaisesRegex(RuntimeError, "Multiple new"):
            await owner.status()
        self.assertIsNone(owner.peer)
        await owner.close()
        self.assertNotIn("RemoveDevice", [event[0] for event in events])

    async def test_open_refuses_power_changes_and_existing_advertising(self):
        for powered, active in ((False, 0), (True, 1), (True, None)):
            with self.subTest(powered=powered, active=active):
                owner, events = await self.open_fixture(powered=powered, active=active)
                self.assertEqual(events, [("bus_disconnect",)])
                self.assertIsNone(owner._lock)

    async def test_uncertain_pairable_change_is_restored_during_failed_open(self):
        owner, events = await self.open_fixture(fail_pairable=True)
        self.assertIn(("pairable", True), events)
        self.assertIn(("pairable", False), events)
        self.assertIn("UnregisterAgent", [event[0] for event in events])
        self.assertIsNone(owner._lock)

    async def open_fixture(self, *, powered=True, active=0, fail_pairable=False):
        owner, events = self.fixture()
        owner.bus = owner.peer = None
        owner._application = owner._advertising = owner._agent = owner._pairable_changed = False
        owner._before_devices = {}

        async def objects():
            return {owner.adapter: {
                "org.bluez.Adapter1": {"Powered": powered, "Pairable": False,
                                       "Address": "11:22:33:44:55:66", "AddressType": "public"},
                "org.bluez.GattManager1": {},
                "org.bluez.LEAdvertisingManager1": {"ActiveInstances": active}}}

        class Bus:
            async def connect(self):
                return self

            def disconnect(self):
                events.append(("bus_disconnect",))

        async def pairable(value):
            events.append(("pairable", value))
            if value and fail_pairable:
                raise RuntimeError("The Pairable reply was lost")

        owner._objects, owner._set_pairable = objects, pairable
        dbus = ModuleType("dbus_next")
        dbus.BusType = SimpleNamespace(SYSTEM=1)
        aio = ModuleType("dbus_next.aio")
        aio.MessageBus = lambda **kwargs: Bus()
        with tempfile.TemporaryDirectory() as temporary, \
                patch.dict(sys.modules, {"dbus_next": dbus, "dbus_next.aio": aio}), \
                patch("amity_harness.ble_keyboard_worker.tempfile.gettempdir", return_value=temporary), \
                patch("amity_harness.ble_keyboard_worker._interfaces",
                      return_value=(owner.path, owner.path + "/advertisement", owner.path + "/agent")):
            with self.assertRaises(RuntimeError):
                await owner.open()
        return owner, events


if __name__ == "__main__":
    unittest.main()
