"""Distributed scan evidence comes only from proven owners and their own wires."""

from copy import deepcopy
from pathlib import Path
from types import SimpleNamespace
import sys
import threading
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import AvrComponent, DUT, Frame, PlaybackComponent, TvComponent, assign_addresses
from amity_harness.discovery import _native_selector, scan_hdmi


class DistributedScanTests(unittest.TestCase):
    def setUp(self):
        self.main, self.member = DUT("amity-dut"), DUT("amity-dut-2")
        self.boot, self.member_boot, self.group = "b" * 32, "d" * 32, "c" * 32
        self.components = []
        for dut in (self.main, self.member):
            tentacle = dut.tentacle(0)
            tv = TvComponent(tentacle.front, "Same TV")
            avr = AvrComponent(tentacle.back, "Same AVR")
            self.components.extend((tv, avr, PlaybackComponent(avr, "Same Player", port=1)))
        assign_addresses(self.components)
        self.session = SimpleNamespace(components=self.components, records=[], _stop=threading.Event(),
                                       assert_healthy=lambda: None)
        self.bed = SimpleNamespace(_entered=True, _closed=False, session=self.session,
                                   duts=(self.main, self.member), members=(self.member,), _member_proofs={})
        self.main._testbed = self.member._testbed = self.bed
        self.proof = SimpleNamespace(main=self.main, candidate=self.member, session=self.session,
                                     device_id=self.member.identity.device_id, boot=self.boot, group=self.group,
                                     member_boot=self.member_boot, _created=True, _closed=False)
        self.bed._member_proofs[self.member.identity.device_id] = self.proof
        self.system = {"octopus_id": self.main.identity.device_id, "consortium_id": self.group,
                       "boot_id": self.boot, "mode": "coordinator", "ready": True,
                       "membership": {"coordinator": True, "authority_ready": True, "member": False,
                                      "peers": [{"octopus_id": self.member.identity.device_id,
                                                 "boot_id": self.member_boot, "enrolled": True,
                                                 "ready": True}]}, "observations": []}
        self.before = {"expected_boot_id": self.boot, "discovering": False,
                       "discovery_generation": 4, "devices": []}
        self.after = {**self.before, "discovery_generation": 5, "devices": []}
        self.trace = []
        self.started = False
        self.submissions = 0
        self.main.get_system = lambda: deepcopy(self.system)
        self.member.get_system = lambda: self.fail("A headless member must never receive an HTTP request")
        self.main.start_hdmi_scan = self.start
        self.main.wait_for = self.wait
        for component in self.components:
            name = {"consortium_id": self.group, "octopus_id": component.dut.identity.device_id,
                    "tentacle_id": component.tentacle, "side": component.direction, "osd_name": component.name}
            runtime = {key: value for key, value in name.items() if key != "osd_name"}
            runtime["logical_address"] = component.address.logical
            self.system["observations"].append({"qualified_name": name, "runtime_address": runtime,
                                                "kind": component.device_type, "resolved": True, "stale": False})
            self.after["devices"].append({"name": _native_selector(name), "title": component.name,
                                           "bus": component.direction, "address": component.address.logical,
                                           "physical_address": component.address.physical, "type": component.device_type})
            initiator = 4 if component.direction == "front" else 0
            for query, response, timestamp in ((0x46, 0x47, 11.0), (0x83, 0x84, 12.0)):
                common = {"component": component.name, "gpio": component.gpio, "transport": "wire"}
                destination = initiator if response == 0x47 else 15
                operands = (component.name.encode("ascii") if response == 0x47 else
                            component.address.physical.to_bytes(2, "big") + bytes([component.device_type]))
                self.trace.extend([
                    {**common, "direction": "rx", "source": initiator, "destination": component.address.logical,
                     "opcode": query, "frame": Frame(initiator, component.address.logical, query).raw.hex(":"),
                     "timestamp": timestamp, "wire_timestamp_ns": int(timestamp * 1e9)},
                    {**common, "direction": "tx", "source": component.address.logical, "destination": destination,
                     "opcode": response, "frame": Frame(component.address.logical, destination, response, operands).raw.hex(":"),
                     "timestamp": timestamp + .1, "wire_timestamp_ns": int((timestamp + .1) * 1e9), "ok": True},
                ])

    def start(self):
        self.started = True
        self.submissions += 1
        self.session.records.extend(self.trace)
        return {"operation_id": "e" * 32, "stage": "complete"}

    def wait(self, predicate, **kwargs):
        state = deepcopy(self.after if self.started else self.before)
        if predicate(state):
            return state
        raise TimeoutError("The scripted evidence does not satisfy the scan assertion")

    def scan(self):
        with patch("amity_harness.discovery.time.monotonic_ns", return_value=10_000_000_000):
            return scan_hdmi(self.main, self.session)

    def test_six_identical_named_devices_resolve_to_each_actual_owner_and_wire(self):
        result = self.scan().assert_contains_only(*self.components)
        self.assertEqual(result.owner_boots, {self.main.identity.device_id: self.boot,
                                              self.member.identity.device_id: self.member_boot})
        self.assertEqual(result.owner_duts, {self.main.identity.device_id: self.main,
                                             self.member.identity.device_id: self.member})
        self.assertIs(result._testbed, self.bed)
        self.assertEqual(len(result.initiators), 4)
        self.assertEqual(self.submissions, 1)
        for component in self.components:
            result.assert_on(component, component.endpoint)
        self.assertEqual(len(result.locations), 6)
        self.assertEqual({item["qualified_name"]["octopus_id"] for item in result.locations},
                         {self.main.identity.device_id, self.member.identity.device_id})

    def test_matching_ui_and_other_owner_wire_cannot_replace_member_exchanges(self):
        member_gpios = {component.gpio for component in self.components[3:]}
        self.trace = [record for record in self.trace if record["gpio"] not in member_gpios]
        result = self.scan()
        result.assert_contains(*self.components[:3])
        with self.assertRaises(TimeoutError):
            result.assert_contains(*self.components[3:])

    def test_virtual_or_prescan_member_traffic_cannot_prove_discovery(self):
        for mutation in ({"transport": "virtual"}, {"wire_timestamp_ns": 1}, {"dropped": True}):
            with self.subTest(mutation=mutation):
                self.setUp()
                member_gpios = {component.gpio for component in self.components[3:]}
                for record in self.trace:
                    if record["gpio"] in member_gpios:
                        record.update(mutation)
                result = self.scan()
                with self.assertRaises(TimeoutError):
                    result.assert_contains(self.components[3])

    def test_changed_member_boot_enrollment_readiness_or_group_rejects_captured_evidence(self):
        for key, value in (("boot_id", "e" * 32), ("ready", False), ("enrolled", False)):
            with self.subTest(key=key):
                self.setUp()
                result = self.scan()
                self.system["membership"]["peers"][0][key] = value
                with self.assertRaises(AssertionError):
                    result.assert_contains(self.components[3])
        self.setUp()
        result = self.scan()
        self.system["consortium_id"] = "e" * 32
        with self.assertRaises(AssertionError):
            result.validate_owners()

    def test_main_reboot_cannot_reuse_member_evidence(self):
        result = self.scan()
        self.system["boot_id"] = "e" * 32
        with self.assertRaises(AssertionError):
            result.validate_owners()

    def test_unconfirmed_retired_changed_or_foreign_proof_cannot_start_scan(self):
        for field, value in (("_created", False), ("_closed", True), ("member_boot", "e" * 32),
                             ("boot", "e" * 32), ("group", "e" * 32), ("session", object()),
                             ("candidate", DUT("amity-dut-2"))):
            with self.subTest(field=field):
                self.setUp()
                setattr(self.proof, field, value)
                with self.assertRaises(AssertionError):
                    self.scan()
                self.assertEqual(self.submissions, 0)

    def test_member_must_remain_in_same_active_testbed_and_explicit_members(self):
        for mutation in (lambda: setattr(self.bed, "_closed", True),
                         lambda: setattr(self.bed, "members", ()),
                         lambda: setattr(self.member, "_testbed", None),
                         lambda: self.bed._member_proofs.clear()):
            with self.subTest(mutation=mutation):
                self.setUp()
                result = self.scan()
                mutation()
                with self.assertRaises(AssertionError):
                    result.validate_owners()

    def test_unsolicited_observed_owner_is_rejected_even_when_requested_equipment_matches(self):
        result = self.scan()
        observation = deepcopy(self.system["observations"][0])
        observation["qualified_name"]["octopus_id"] = "e" * 32
        observation["runtime_address"]["octopus_id"] = "e" * 32
        self.system["observations"].append(observation)
        row = deepcopy(self.after["devices"][0])
        row["name"] = _native_selector(observation["qualified_name"])
        result.status["devices"].append(row)
        with self.assertRaisesRegex(AssertionError, "unsolicited"):
            result.assert_contains(self.components[0])

    def test_misreported_owner_cannot_use_same_labels_addresses_and_type(self):
        result = self.scan()
        for observation in self.system["observations"][3:]:
            observation["qualified_name"]["octopus_id"] = self.main.identity.device_id
            observation["runtime_address"]["octopus_id"] = self.main.identity.device_id
        with self.assertRaises(AssertionError):
            result.assert_contains_only(*self.components)

    def test_scan_gpio_cannot_be_shared_by_two_dut_owners(self):
        object.__setattr__(self.components[3].endpoint, "gpio", self.components[0].gpio)
        with self.assertRaisesRegex(AssertionError, "GPIO"):
            self.scan()
        self.assertEqual(self.submissions, 0)


if __name__ == "__main__":
    unittest.main()
