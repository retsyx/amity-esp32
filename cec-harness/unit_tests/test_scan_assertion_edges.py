"""Independent adversarial checks for UI identity snapshot correlation."""

from copy import deepcopy
import hashlib
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import (AvrComponent, DUT, Frame, PlaybackComponent, ScanResult,
                           TvComponent, assign_addresses)


class ScanAssertionSnapshotTests(unittest.TestCase):
    def fixture(self, *, saved=False):
        dut = DUT("amity-dut")
        tentacle = dut.tentacle(0)
        tv = TvComponent(tentacle.front, "Edge TV")
        avr = AvrComponent(tentacle.back, "Edge AVR")
        player = PlaybackComponent(avr, "Edge Player", port=1)
        components = (tv, avr, player)
        assign_addresses(components)
        boot, consortium = "b" * 32, "c" * 32
        system = {"octopus_id": dut.identity.device_id, "consortium_id": consortium,
                  "boot_id": boot, "observations": []}
        config = {"consortium_id": consortium, "config_revision": "4",
                  "presentation_revision": "9", "components": []}
        rows, records = [], []
        for index, component in enumerate(components, start=1):
            qualified = {"consortium_id": consortium, "octopus_id": dut.identity.device_id,
                         "tentacle_id": 0, "side": component.direction, "osd_name": component.name}
            runtime = {key: value for key, value in qualified.items() if key != "osd_name"}
            runtime["logical_address"] = component.address.logical
            encoded = json.dumps(qualified, ensure_ascii=False, separators=(",", ":")).encode()
            selector = "native:" + hashlib.sha256(encoded).hexdigest()
            if saved:
                identity = str(index) * 32
                config["components"].append({"component_id": identity, "qualified_names": [qualified]})
                selector = "component:" + identity
            rows.append({"name": selector, "title": f"Saved user label {index}" if saved else component.name,
                         # Deliberately misleading display text must never be
                         # used to establish the actual owner/adapter/side.
                         "location": "Different room · HDMI adapter 4 · unknown connection",
                         "bus": component.direction, "address": component.address.logical,
                         "physical_address": component.address.physical, "type": component.device_type})
            system["observations"].append({"qualified_name": qualified, "runtime_address": runtime,
                                            "kind": component.device_type, "resolved": True, "stale": False})
            initiator = 4 if component.direction == "front" else 0
            for offset, (query, reply) in enumerate(((0x46, 0x47), (0x83, 0x84))):
                common = {"component": component.name, "gpio": component.gpio, "transport": "wire"}
                timestamp = 2.0 + index + offset * 0.2
                destination = initiator if reply == 0x47 else 15
                operands = (component.name.encode("ascii") if reply == 0x47 else
                            component.address.physical.to_bytes(2, "big") + bytes([component.device_type]))
                records.extend([
                    {**common, "direction": "rx", "opcode": query, "source": initiator,
                     "destination": component.address.logical, "timestamp": timestamp,
                     "frame": Frame(initiator, component.address.logical, query).raw.hex(":"),
                     "wire_timestamp_ns": int(timestamp * 1e9)},
                    {**common, "direction": "tx", "opcode": reply, "source": component.address.logical,
                     "destination": destination, "timestamp": timestamp + 0.1,
                     "frame": Frame(component.address.logical, destination, reply, operands).raw.hex(":"),
                     "wire_timestamp_ns": int((timestamp + 0.1) * 1e9), "ok": True},
                ])
        status = {"discovering": False, "discovery_generation": 2, "expected_boot_id": boot,
                  "expected_config_revision": "4", "expected_presentation_revision": "9", "devices": rows}
        dut.get_system = lambda: deepcopy(system)

        def request(path, *args, **kwargs):
            if path != "/api/v2/config":
                raise AssertionError("The mapping must use only the existing UI config route")
            return deepcopy(config)

        dut.request = request
        result = ScanResult(dut, expected=components, devices=deepcopy(rows), status=status,
                            records=tuple(records), started=1_000_000_000,
                            initiators={tv.gpio: 4, avr.gpio: 0}, _boot=boot, _generation=1)
        return result, components, tentacle, system, config

    def test_saved_labels_and_human_locations_do_not_define_scoped_identity(self):
        result, components, tentacle, _, _ = self.fixture(saved=True)
        self.assertIs(result.assert_contains_only(*components), result)
        for component, endpoint in zip(components, (tentacle.front, tentacle.back, tentacle.back)):
            self.assertIs(result.assert_on(component, endpoint), result)
        self.assertEqual([entry["qualified_name"]["osd_name"] for entry in result.locations],
                         [component.name for component in components])

    def test_observations_after_a_reboot_cannot_reuse_previous_scan_wire_evidence(self):
        for assertion in ("exact", "port"):
            with self.subTest(assertion=assertion):
                result, components, tentacle, system, _ = self.fixture()
                system["boot_id"] = "d" * 32
                with self.assertRaises(AssertionError):
                    if assertion == "exact":
                        result.assert_contains_only(*components)
                    else:
                        result.assert_on(components[0], tentacle.front)

    def test_changed_saved_selector_revisions_cannot_rebind_old_ui_rows(self):
        for revision in ("config_revision", "presentation_revision"):
            with self.subTest(revision=revision):
                result, components, tentacle, _, config = self.fixture(saved=True)
                config[revision] = "10"
                with self.assertRaises(AssertionError):
                    result.assert_contains_only(*components)
                with self.assertRaises(AssertionError):
                    result.assert_on(components[0], tentacle.front)

    def test_missing_snapshot_fences_fail_closed_instead_of_guessing_consistency(self):
        result, components, _, system, _ = self.fixture()
        del system["boot_id"]
        with self.assertRaises(AssertionError):
            result.assert_contains_only(*components)
        result, components, _, _, config = self.fixture(saved=True)
        del config["presentation_revision"]
        with self.assertRaises(AssertionError):
            result.assert_contains_only(*components)


if __name__ == "__main__":
    unittest.main()
