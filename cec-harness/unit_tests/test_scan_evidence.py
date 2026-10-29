"""Host checks of scan evidence; no additional physical functional scenario."""

from copy import deepcopy
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import CecAddress, Frame
from amity_harness.discovery import _fresh_discovery, _matching_devices, _wire_initiators


class ScanEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.component = SimpleNamespace(name="ScanTV-test", gpio=23, direction="front",
                                         address=CecAddress(0, 0), device_type=0, vendor_id=1)
        self.started_ns = 10_000_000_000
        self.records = self.exchanges(self.component, 4)
        self.row = {"title": self.component.name, "bus": "front", "address": 0,
                    "physical_address": 0, "type": 0, "vendor_id": 1}

    def exchanges(self, component, initiator):
        records = []
        for index, (query, response) in enumerate(((0x46, 0x47), (0x83, 0x84))):
            timestamp = 11.0 + index
            common = {"component": component.name, "gpio": component.gpio}
            destination = initiator if response == 0x47 else 15
            operands = (component.name.encode("ascii") if response == 0x47 else
                        component.address.physical.to_bytes(2, "big") + bytes([component.device_type]))
            records.append({**common, "timestamp": timestamp, "direction": "rx",
                            "opcode": query, "source": initiator,
                            "destination": component.address.logical,
                            "frame": Frame(initiator, component.address.logical, query).raw.hex(":"),
                            "wire_timestamp_ns": int(timestamp * 1_000_000_000)})
            records.append({**common, "timestamp": timestamp + 0.1, "direction": "tx",
                            "opcode": response, "source": component.address.logical,
                            "destination": destination, "ok": True,
                            "frame": Frame(component.address.logical, destination, response, operands).raw.hex(":"),
                            "wire_timestamp_ns": int((timestamp + 0.1) * 1_000_000_000)})
        return records

    def fresh(self, records):
        return _fresh_discovery(records, self.component, 4, self.started_ns)

    def test_success_needs_both_name_and_physical_identity_exchanges(self):
        self.assertTrue(self.fresh(self.records))
        for index in range(len(self.records)):
            with self.subTest(missing=index):
                self.assertFalse(self.fresh(self.records[:index] + self.records[index + 1:]))

    def test_identity_reply_requires_exact_wire_name_address_type_and_frame_shape(self):
        for index, payloads in ((1, (b"Wrong TV", b"", self.component.name.encode("ascii") + b"\0")),
                                (3, (b"\x10\x00\x00", b"\x00\x00\x04", b"\x00\x00", b"\x00\x00\x00\x00"))):
            for operands in payloads:
                records = deepcopy(self.records)
                record = records[index]
                record["frame"] = Frame(record["source"], record["destination"], record["opcode"], operands).raw.hex(":")
                with self.subTest(index=index, operands=operands):
                    self.assertFalse(self.fresh(records))
        for index in (1, 3):
            for raw in (None, "invalid", "", "00", "44:47:54:56"):
                records = deepcopy(self.records)
                records[index]["frame"] = raw
                with self.subTest(index=index, raw=raw):
                    self.assertFalse(self.fresh(records))

    def test_identity_query_requires_original_wire_header_opcode_and_no_operands(self):
        for index in (0, 2):
            for raw in (None, "invalid", "", "00", "40:8f", self.records[index]["frame"] + ":00"):
                records = deepcopy(self.records)
                records[index]["frame"] = raw
                with self.subTest(index=index, raw=raw):
                    self.assertFalse(self.fresh(records))

    def test_cached_or_late_delivered_prescan_requests_do_not_pass(self):
        old = deepcopy(self.records)
        for record in old:
            record["timestamp"] -= 10
        self.assertFalse(self.fresh(old))
        late = deepcopy(self.records)
        for record in late:
            if record["direction"] == "rx":
                record["wire_timestamp_ns"] = self.started_ns - 1
        self.assertFalse(self.fresh(late))

    def test_requests_from_other_device_or_wire_do_not_pass(self):
        for field, value in (("source", 8), ("destination", 5), ("gpio", 24),
                             ("component", "different-component")):
            records = deepcopy(self.records)
            for record in records:
                if record["direction"] == "rx":
                    record[field] = value
            with self.subTest(field=field):
                self.assertFalse(self.fresh(records))

    def test_failed_dropped_or_earlier_replies_do_not_pass(self):
        for field, value in (("ok", False), ("dropped", True), ("timestamp", 10.5),
                             ("source", 5), ("destination", 8)):
            records = deepcopy(self.records)
            records[1][field] = value
            with self.subTest(field=field):
                self.assertFalse(self.fresh(records))

    def test_dropped_receive_is_not_discovery_evidence(self):
        records = deepcopy(self.records)
        records[0]["dropped"] = True
        self.assertFalse(self.fresh(records))

    def test_matching_status_needs_complete_identity(self):
        self.assertEqual(_matching_devices({"devices": [self.row]}, [self.component]), [self.row])
        for key, value in (("title", "StaleTV"), ("bus", "back"), ("address", 5),
                           ("physical_address", 4096), ("type", 5)):
            row = {**self.row, key: value}
            with self.subTest(key=key):
                self.assertIsNone(_matching_devices({"devices": [row]}, [self.component]))

    def test_duplicate_or_missing_status_is_not_success(self):
        self.assertIsNone(_matching_devices({"devices": [self.row, dict(self.row)]}, [self.component]))
        self.assertIsNone(_matching_devices({"devices": []}, [self.component]))

    def test_initial_scan_does_not_require_optional_vendor_discovery(self):
        # Name, physical address and type establish the requested scan result.
        # The baseline can omit the TV's vendor query on this isolated wire.
        row = {**self.row, "vendor_id": 0}
        self.assertEqual(_matching_devices({"devices": [row]}, [self.component]), [row])
        self.assertTrue(self.fresh(self.records))

    def test_wire_identity_uses_real_queries_when_ui_aggregate_is_unregistered(self):
        avr = SimpleNamespace(name="ScanAVR-test", gpio=24, direction="back",
                              address=CecAddress(5, 0x1000), device_type=5, vendor_id=1)
        player = SimpleNamespace(name="ScanPlay-test", gpio=24, direction="back",
                                 address=CecAddress(4, 0x1100), device_type=4, vendor_id=1)
        components = [self.component, avr, player]
        records = self.records + self.exchanges(avr, 0) + self.exchanges(player, 0)
        rows = [{"title": component.name, "bus": component.direction,
                 "address": component.address.logical, "physical_address": component.address.physical,
                 "type": component.device_type} for component in components]
        state = {"front_address": 15, "back_address": 15, "devices": rows}
        initiators = _wire_initiators(records, components, self.started_ns)
        self.assertEqual(initiators, {23: 4, 24: 0})
        self.assertEqual(_matching_devices(state, components), rows)
        for component in components:
            self.assertTrue(_fresh_discovery(records, component, initiators[component.gpio], self.started_ns))
        self.assertFalse(_fresh_discovery(records, self.component, state["front_address"], self.started_ns))

    def test_ambiguous_wire_source_is_rejected(self):
        other_query = {**self.records[0], "source": 8,
                       "frame": Frame(8, self.component.address.logical, 0x46).raw.hex(":")}
        self.assertEqual(_wire_initiators(self.records + [other_query], [self.component], self.started_ns), {})

    def test_initiator_cannot_be_inferred_without_fresh_directed_query(self):
        cases = {
            "replies only": [record for record in self.records if record["direction"] == "tx"],
            "old wire timestamp": [{**record, "wire_timestamp_ns": self.started_ns - 1}
                                   for record in self.records],
            "old observer timestamp": [{**record, "timestamp": 9} for record in self.records],
            "dropped": [{**record, "dropped": True} for record in self.records],
            "unrelated opcode": [{**record, "opcode": 0x8f} for record in self.records],
            "self source": [{**record, "source": 0} for record in self.records],
            "unregistered source": [{**record, "source": 15} for record in self.records],
            "wrong target": [{**record, "destination": 5} for record in self.records],
            "other wire": [{**record, "gpio": 24} for record in self.records],
            "missing wire bytes": [{**record, "frame": None} for record in self.records],
            "query operands": [{**record, "frame": record["frame"] + ":00"} for record in self.records],
        }
        for label, records in cases.items():
            with self.subTest(label=label):
                self.assertEqual(_wire_initiators(records, [self.component], self.started_ns), {})

    def test_virtual_transport_never_counts_as_physical_scan_evidence(self):
        # Even a malformed/imported record with plausible physical timestamps
        # must not turn internal component traffic into proof of DUT discovery.
        virtual = [{**record, "transport": "virtual"} for record in self.records]
        self.assertFalse(self.fresh(virtual))
        self.assertEqual(_wire_initiators(virtual, [self.component], self.started_ns), {})
        for direction in ("rx", "tx"):
            mixed = [{**record, "transport": "virtual" if record["direction"] == direction else "wire"}
                     for record in self.records]
            with self.subTest(virtual_direction=direction):
                self.assertFalse(self.fresh(mixed))
                if direction == "rx":
                    self.assertEqual(_wire_initiators(mixed, [self.component], self.started_ns), {})


if __name__ == "__main__":
    unittest.main()
