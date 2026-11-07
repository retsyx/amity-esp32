"""Fresh multi-owner checks share one snapshot per verification pass."""

from collections import Counter
from copy import deepcopy
from pathlib import Path
import sys
import threading
import time
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import AvrComponent, DUT, Frame, Key, PlaybackComponent, TvComponent, assign_addresses
from amity_harness.ble_input import BLEInputEvidence
from amity_harness.discovery import ScanResult


class FourOwners:
    def __init__(self):
        self.owners = tuple(DUT(name) for name in ("amity-dut", "amity-dut-2", "amity-dut-3", "amity-dut-4"))
        self.main = self.owners[0]
        self.boots = {owner.identity.device_id: digit * 32
                      for owner, digit in zip(self.owners, "abcd")}
        self.group, self.calls = "e" * 32, []
        components = []
        for index, owner in enumerate(self.owners):
            connector = owner.tentacle(0)
            tv, avr = TvComponent(connector.front, f"TV{index}"), AvrComponent(connector.back, f"AVR{index}")
            components.extend((tv, avr, PlaybackComponent(avr, f"Player{index}a", port=1),
                               PlaybackComponent(avr, f"Player{index}b", port=2, address_type="tuner")))
        self.components = tuple(components)
        assign_addresses(self.components)
        self.session = SimpleNamespace(components=self.components, records=[], _stop=threading.Event(),
                                       _lock=threading.RLock(), assert_healthy=lambda: None,
                                       power_changed=lambda *args: None)
        self.bed = SimpleNamespace(session=self.session, _entered=True, _closed=False,
                                   duts=self.owners, members=self.owners[1:], _member_proofs={})
        for owner in self.owners:
            owner._testbed = self.bed
        for component in self.components:
            component._session = self.session
        for owner in self.owners[1:]:
            self.bed._member_proofs[owner.identity.device_id] = SimpleNamespace(
                main=self.main, candidate=owner, session=self.session, device_id=owner.identity.device_id,
                boot=self.boots[self.main.identity.device_id], group=self.group,
                member_boot=self.boots[owner.identity.device_id], _created=True, _closed=False)
        self.system = {"octopus_id": self.main.identity.device_id, "boot_id": self.boots[self.main.identity.device_id],
                       "consortium_id": self.group, "mode": "coordinator", "ready": True,
                       "membership": {"coordinator": True, "authority_ready": True, "member": False,
                                      "peers": [{"octopus_id": owner.identity.device_id,
                                                 "boot_id": self.boots[owner.identity.device_id],
                                                 "enrolled": True, "ready": True} for owner in self.owners[1:]]}}
        self.state = {"expected_boot_id": self.system["boot_id"], "current_activity": 0,
                      "activity_settling": False, "expected_config_revision": "1",
                      "expected_presentation_revision": "1"}
        self.scan = ScanResult(self.main, expected=self.components, _session=self.session,
                               _testbed=self.bed, _boot=self.system["boot_id"], _group=self.group,
                               owner_boots=dict(self.boots),
                               owner_duts={owner.identity.device_id: owner for owner in self.owners},
                               initiators={component.gpio: 4 if component.direction == "front" else 0
                                           for component in self.components})
        self.main.last_scan = self.scan

        def system():
            self.calls.append("GET /api/system")
            return deepcopy(self.system)

        def state():
            self.calls.append("GET /api/activity-editor/status")
            return deepcopy(self.state)

        def request(path, data=None, **kwargs):
            assert path == "/api/key" and data is not None
            self.calls.append("POST /api/key")
            return {"ok": True}

        def record(component, direction, frame, *, transport="wire", **extra):
            self.session.records.append({"component": component.name, "gpio": component.gpio,
                                         "direction": direction, "transport": transport,
                                         "frame": frame.raw.hex(":"), "source": frame.source,
                                         "destination": frame.destination, "opcode": frame.opcode,
                                         "wire_timestamp_ns": frame.timestamp_ns, **extra})

        self.main.get_system, self.main.get_state, self.main.request = system, state, request
        self.session._record = record


class ValidationRequestTests(unittest.TestCase):
    def evidence(self, fixture):
        evidence = BLEInputEvidence(fixture.main, fixture.scan, *fixture.components)
        evidence.__enter__()
        self.addCleanup(evidence.__exit__, RuntimeError, RuntimeError("validator-only fixture"), None)
        return evidence

    def test_four_owner_ble_validation_uses_one_system_and_one_activity_read_per_pass(self):
        fixture = FourOwners()
        evidence = self.evidence(fixture)
        expected = Counter({"GET /api/system": 1, "GET /api/activity-editor/status": 1})
        self.assertEqual(Counter(fixture.calls), expected)
        fixture.calls.clear()
        evidence._validate()
        self.assertEqual(Counter(fixture.calls), expected)

    def test_new_validation_pass_rejects_main_and_every_member_boot_change(self):
        for index in range(4):
            with self.subTest(owner=index):
                fixture = FourOwners()
                evidence = self.evidence(fixture)
                fixture.calls.clear()
                if index:
                    fixture.system["membership"]["peers"][index - 1]["boot_id"] = "f" * 32
                else:
                    fixture.system["boot_id"] = "f" * 32
                with self.assertRaises(AssertionError):
                    evidence._validate()
                self.assertEqual(fixture.calls, ["GET /api/system"])

    def test_snapshot_reuse_preserves_component_session_and_initiator_fences(self):
        for fault in ("session", "bed", "scan", "initiator", "owner"):
            with self.subTest(fault=fault):
                fixture = FourOwners()
                evidence = self.evidence(fixture)
                component = fixture.components[-1]
                if fault == "session":
                    component._session = object()
                elif fault == "bed":
                    component.dut._testbed = object()
                elif fault == "scan":
                    fixture.main.last_scan = object()
                elif fault == "initiator":
                    fixture.scan.initiators[component.gpio] = 8
                else:
                    fixture.scan.owner_duts[component.dut.identity.device_id] = DUT("amity-dut-4")
                with self.assertRaises(AssertionError):
                    evidence._validate()

    def test_member_key_action_uses_fresh_pre_and_post_wire_snapshots_without_duplicates(self):
        fixture = FourOwners()
        action = fixture.main.press_key(Key.RIGHT)
        component = fixture.components[-1]
        stamp = time.monotonic_ns()
        component.receive(Frame(0, component.address.logical, 0x44, bytes([Key.RIGHT]), stamp), transport="wire")
        component.receive(Frame(0, component.address.logical, 0x45, timestamp_ns=stamp + 1), transport="wire")
        action.assert_received_by(component)
        self.assertEqual(Counter(fixture.calls), Counter({"GET /api/system": 3,
                                                          "GET /api/activity-editor/status": 3,
                                                          "POST /api/key": 1}))

    def test_key_post_wire_pass_rejects_each_owner_reboot_after_preflight(self):
        for index in range(4):
            with self.subTest(owner=index):
                fixture = FourOwners()
                action = fixture.main.press_key(Key.RIGHT)
                component = fixture.components[-1]
                stamp = time.monotonic_ns()
                component.receive(Frame(0, component.address.logical, 0x44, bytes([Key.RIGHT]), stamp), transport="wire")
                component.receive(Frame(0, component.address.logical, 0x45, timestamp_ns=stamp + 1), transport="wire")
                read_system = fixture.main.get_system

                def system():
                    if fixture.calls.count("GET /api/system") == 2:
                        if index:
                            fixture.system["membership"]["peers"][index - 1]["boot_id"] = "f" * 32
                        else:
                            fixture.system["boot_id"] = "f" * 32
                    return read_system()

                fixture.main.get_system = system
                with self.assertRaises(AssertionError):
                    action.assert_received_by(component)
                self.assertEqual(fixture.calls.count("GET /api/system"), 3)


if __name__ == "__main__":
    unittest.main()
