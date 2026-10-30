"""Scan assertions bind UI identities to actual typed equipment locations."""

from copy import deepcopy
import hashlib
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import AvrComponent, DUT, Frame, PlaybackComponent, TvComponent, assign_addresses
from amity_harness.discovery import ScanResult


def native_selector(name):
    ordered = {key: name[key] for key in ("consortium_id", "octopus_id", "tentacle_id", "side", "osd_name")}
    return "native:" + hashlib.sha256(json.dumps(ordered, separators=(",", ":"), ensure_ascii=False).encode()).hexdigest()


class ScanAssertionTests(unittest.TestCase):
    def setUp(self):
        self.dut = DUT("amity-dut")
        connector = self.dut.tentacle(0)
        self.tv = TvComponent(connector.front, "Scan TV")
        self.avr = AvrComponent(connector.back, "Scan AVR")
        self.player = PlaybackComponent(self.avr, "Scan Player", port=1)
        self.components = (self.tv, self.avr, self.player)
        assign_addresses(self.components)
        self.group, self.boot = "c" * 32, "b" * 32
        self.rows, observations, records = [], [], []
        for component in self.components:
            qname = {"consortium_id": self.group, "octopus_id": self.dut.identity.device_id,
                     "tentacle_id": component.tentacle, "side": component.direction, "osd_name": component.name}
            runtime = {key: qname[key] for key in ("consortium_id", "octopus_id", "tentacle_id", "side")}
            runtime["logical_address"] = component.address.logical
            observations.append({"qualified_name": qname, "runtime_address": runtime,
                                 "kind": component.device_type, "resolved": True, "stale": False})
            self.rows.append({"name": native_selector(qname), "title": component.name, "bus": component.direction,
                              "address": component.address.logical, "physical_address": component.address.physical,
                              "type": component.device_type, "location": "Deliberately incorrect English location"})
            initiator = 4 if component.direction == "front" else 0
            for query, response, timestamp in ((0x46, 0x47, 11.0), (0x83, 0x84, 12.0)):
                common = {"component": component.name, "gpio": component.gpio, "transport": "wire"}
                destination = initiator if response == 0x47 else 15
                operands = (component.name.encode("ascii") if response == 0x47 else
                            component.address.physical.to_bytes(2, "big") + bytes([component.device_type]))
                records.append({**common, "direction": "rx", "source": initiator,
                                "destination": component.address.logical, "opcode": query,
                                "frame": Frame(initiator, component.address.logical, query).raw.hex(":"),
                                "timestamp": timestamp, "wire_timestamp_ns": int(timestamp * 1e9)})
                records.append({**common, "direction": "tx", "source": component.address.logical,
                                "destination": destination, "opcode": response,
                                "frame": Frame(component.address.logical, destination, response, operands).raw.hex(":"),
                                "timestamp": timestamp + .1, "wire_timestamp_ns": int((timestamp + .1) * 1e9), "ok": True})
        self.system = {"octopus_id": self.dut.identity.device_id, "consortium_id": self.group,
                       "boot_id": self.boot, "observations": observations, "private": "must not enter locations"}
        self.config = {"consortium_id": self.group, "config_revision": "7", "presentation_revision": "3", "components": []}
        self.requests = []
        self.dut.get_system = lambda: deepcopy(self.system)

        def request(path):
            self.requests.append(path)
            self.assertEqual(path, "/api/config")
            return deepcopy(self.config)

        self.dut.request = request
        status = {"devices": self.rows, "discovering": False, "discovery_generation": 5,
                  "expected_boot_id": self.boot, "expected_config_revision": "7", "expected_presentation_revision": "3"}
        self.result = ScanResult(self.dut, expected=self.components, devices=self.rows, status=status,
                                 records=tuple(records), started=10_000_000_000,
                                 initiators={self.tv.gpio: 4, self.avr.gpio: 0}, _boot=self.boot, _generation=4)

    def persist_tv(self, *, label="Living room display"):
        identity = "1" * 32
        self.rows[0]["name"] = "component:" + identity
        self.rows[0]["title"] = label
        self.config["components"] = [{"component_id": identity,
                                      "qualified_names": [deepcopy(self.system["observations"][0]["qualified_name"])]}]

    def test_native_exact_set_and_observed_locations_ignore_english_location(self):
        self.assertIs(self.result.assert_contains_only(*self.components), self.result)
        for component in self.components:
            self.assertIs(self.result.assert_on(component, component.endpoint), self.result)
        self.assertEqual(len(self.result.locations), 3)
        self.assertTrue(all(set(location) == {"component", "qualified_name", "runtime_address"}
                            for location in self.result.locations))
        self.assertFalse(self.requests)  # Native observations need no config read.
        self.result.assert_on(self.tv, self.tv.endpoint)
        self.assertEqual(len(self.result.locations), 3)

    def test_cached_matching_ui_cannot_hide_changed_wire_identity_replies(self):
        original = deepcopy(self.result.records)
        for opcode, operands in ((0x47, b"Wrong AVR"), (0x84, b"\x20\x00\x05"),
                                 (0x84, b"\x10\x00\x04")):
            records = deepcopy(original)
            for record in records:
                if (record["component"] == self.avr.name and record["direction"] == "tx"
                        and record["opcode"] == opcode):
                    record["frame"] = Frame(record["source"], record["destination"], opcode, operands).raw.hex(":")
            self.result.records = records
            for assertion in (lambda: self.result.assert_contains(self.avr),
                              lambda: self.result.assert_contains_only(*self.components),
                              lambda: self.result.assert_on(self.avr, self.avr.endpoint)):
                with self.subTest(opcode=opcode, operands=operands), self.assertRaises(AssertionError):
                    assertion()

    def test_exact_set_rejects_extra_rows_including_other_owners(self):
        extra = deepcopy(self.rows[0])
        extra["name"] = "native:" + "f" * 64
        self.rows.append(extra)
        with self.assertRaises(AssertionError):
            self.result.assert_contains_only(*self.components)

    def test_exact_set_rejects_missing_or_duplicate_rows(self):
        original = deepcopy(self.rows)
        for rows in (original[:-1], [original[0], deepcopy(original[0]), *original[1:]]):
            self.result.status["devices"] = rows
            with self.subTest(rows=rows), self.assertRaises(AssertionError):
                self.result.assert_contains_only(*self.components)
        with self.assertRaises(AssertionError):
            self.result.assert_contains_only(self.tv, self.tv)

    def test_location_uses_observed_owner_adapter_and_side(self):
        for key, value in (("octopus_id", "d" * 32), ("tentacle_id", 1), ("side", "back")):
            observations = deepcopy(self.system["observations"])
            qname = observations[0]["qualified_name"]
            qname[key] = value
            observations[0]["runtime_address"][key] = value
            self.system["observations"] = observations
            self.rows[0]["name"] = native_selector(qname)
            with self.subTest(key=key), self.assertRaises(AssertionError):
                self.result.assert_on(self.tv, self.tv.endpoint)
            self.setUp()
        with self.assertRaises(AssertionError):
            self.result.assert_on(self.tv, self.dut.tentacle(0).back)

    def test_correct_display_label_cannot_override_wrong_typed_osd_identity(self):
        qname = self.system["observations"][0]["qualified_name"]
        qname["osd_name"] = "Another TV"
        self.rows[0]["name"] = native_selector(qname)
        for assertion in (lambda: self.result.assert_on(self.tv, self.tv.endpoint),
                          lambda: self.result.assert_contains_only(*self.components)):
            with self.assertRaises(AssertionError):
                assertion()

    def test_wrong_or_missing_native_selector_fails_despite_matching_label(self):
        for selector in ("native:" + "0" * 64, "native:invalid", "unknown", None):
            self.rows[0]["name"] = selector
            with self.subTest(selector=selector), self.assertRaises(AssertionError):
                self.result.assert_contains_only(*self.components)
        del self.rows[0]["name"]
        with self.assertRaises(AssertionError):
            self.result.assert_on(self.tv, self.tv.endpoint)

    def test_runtime_port_logical_address_and_kind_must_match_ui_row(self):
        for key, value in (("consortium_id", "d" * 32), ("octopus_id", "d" * 32),
                           ("tentacle_id", 1), ("side", "back"), ("logical_address", 8)):
            original = deepcopy(self.system["observations"][0]["runtime_address"])
            self.system["observations"][0]["runtime_address"][key] = value
            with self.subTest(key=key), self.assertRaises(AssertionError):
                self.result.assert_on(self.tv, self.tv.endpoint)
            self.system["observations"][0]["runtime_address"] = original
        self.system["observations"][0]["kind"] = 4
        with self.assertRaises(AssertionError):
            self.result.assert_on(self.tv, self.tv.endpoint)

    def test_stale_unresolved_missing_or_duplicate_observation_cannot_pass(self):
        original = deepcopy(self.system["observations"])
        for changed in ({"stale": True}, {"resolved": False}, {"stale": None}, {"runtime_address": None}):
            self.system["observations"] = deepcopy(original)
            self.system["observations"][0].update(changed)
            with self.subTest(changed=changed), self.assertRaises(AssertionError):
                self.result.assert_on(self.tv, self.tv.endpoint)
        for observations in (original[1:], [*original, deepcopy(original[0])]):
            self.system["observations"] = observations
            with self.subTest(observations=observations), self.assertRaises(AssertionError):
                self.result.assert_contains_only(*self.components)

    def test_stale_history_does_not_ambiguate_one_current_observation(self):
        old = deepcopy(self.system["observations"][0])
        old.update(stale=True, resolved=False)
        old["runtime_address"]["logical_address"] = 8
        self.system["observations"].insert(0, old)
        self.result.assert_contains_only(*self.components)
        self.result.assert_on(self.tv, self.tv.endpoint)
        self.assertEqual(self.result.locations[0]["runtime_address"]["logical_address"], 0)

    def test_persisted_label_alias_resolves_the_saved_qualified_osd(self):
        self.persist_tv()
        self.result.assert_contains(*self.components)
        self.result.assert_contains_only(*self.components)
        self.result.assert_on(self.tv, self.tv.endpoint)
        self.assertTrue(self.requests)
        self.assertEqual(set(self.requests), {"/api/config"})
        self.assertEqual(self.result.locations[0]["qualified_name"]["osd_name"], self.tv.name)

    def test_unknown_persisted_id_or_multiple_qualified_names_are_rejected(self):
        self.persist_tv(label=self.tv.name)
        saved = deepcopy(self.config["components"])
        self.rows[0]["name"] = "component:" + "2" * 32
        with self.assertRaises(AssertionError):
            self.result.assert_contains_only(*self.components)
        self.rows[0]["name"] = "component:" + "1" * 32
        self.config["components"][0]["qualified_names"].append(deepcopy(saved[0]["qualified_names"][0]))
        with self.assertRaises(AssertionError):
            self.result.assert_on(self.tv, self.tv.endpoint)

    def test_numeric_booleans_and_malformed_typed_names_are_rejected(self):
        for key in ("address", "physical_address", "type"):
            original = self.rows[0][key]
            self.rows[0][key] = False
            with self.subTest(key=key), self.assertRaises(AssertionError):
                self.result.assert_contains_only(*self.components)
            self.rows[0][key] = original
        self.system["observations"][0]["qualified_name"]["tentacle_id"] = False
        with self.assertRaises(AssertionError):
            self.result.assert_on(self.tv, self.tv.endpoint)

    def test_invalid_endpoint_type_does_not_turn_a_string_into_location_evidence(self):
        with self.assertRaises(TypeError):
            self.result.assert_on(self.tv, "amity-dut / HDMI 0 / front")


if __name__ == "__main__":
    unittest.main()
