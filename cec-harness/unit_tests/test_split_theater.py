"""A name gate proves a real bounded lookup without blocking responder work."""

from copy import deepcopy
from dataclasses import replace
from pathlib import Path
import sys
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import Frame, Hooks, TxResult
from amity_harness.activity import ActivityAction
from amity_harness.split_theater import LookupNameGate
from amity_harness.testbed import _CONTROL_OBSERVATION_FIELDS
from test_component_power_assertions import distributed_activity_fixture


class GateFixture:
    def __init__(self, *, member=False):
        self.f = distributed_activity_fixture()
        self.source = self.f.member_player if member else self.f.player
        self.initial = self.f.dut.last_scan
        self.tv, self.avr = ((self.f.tv, self.f.avr) if member
                             else (self.f.member_tv, self.f.member_avr))
        self.transmitted = []

        def transmit(component, frame):
            self.transmitted.append(frame)
            return TxResult(frame, 1, time.monotonic_ns())

        self.f.session.transmit = transmit
        self.gate = LookupNameGate(self.source, self.initial,
                                   progress=(self.tv, self.avr))

    def missing(self):
        f, source = self.f, self.source
        self.status = deepcopy(self.initial.status)
        self.status["devices"] = [row for row in f.source.rows if row["title"] != source.name]
        self.status["activity_settling"] = False
        self.status["discovery_generation"] += 1
        f.source.system["observations"] = [row for row in f.source.system["observations"]
            if not (row["qualified_name"]["osd_name"] == source.name
                    and row["qualified_name"]["octopus_id"] == source.dut.identity.device_id)]
        self.scan = replace(self.initial, devices=deepcopy(self.status["devices"]),
            status=deepcopy(self.status), started=time.monotonic_ns(),
            initiators=dict(self.initial.initiators), owner_boots=dict(self.initial.owner_boots),
            owner_duts=dict(self.initial.owner_duts))
        f.dut.last_scan = self.scan
        f.dut.get_state = lambda: deepcopy(self.status)
        self.gate.set_missing_scan(self.scan)
        self.action = ActivityAction(f.dut, f.session, time.monotonic_ns(), self.scan._boot,
            dict(self.scan.initiators), "start", "f" * 32, "e" * 32,
            scan=self.scan, testbed=f.bed, owner_boots=dict(self.scan.owner_boots),
            owner_duts=dict(self.scan.owner_duts))
        return self

    def query(self, opcode, *, timestamp=None, transport="wire", source=None,
              destination=None, operands=b""):
        frame = Frame(self.scan.initiators[self.source.gpio] if source is None else source,
            self.source.address.logical if destination is None else destination,
            opcode, operands, time.monotonic_ns() if timestamp is None else timestamp)
        self.source.receive(frame, transport=transport)
        return frame

    def pending(self):
        self.status.update(discovering=True, activity_settling=True)
        self.query(0x83)
        self.query(0x46)
        return self

    def b_progress(self, *, changed=None):
        for kind, component, destination, opcode, operands in (
                ("wake", self.tv, self.tv.address.logical, 0x04, b""),
                ("route", self.tv, 15, 0x82, (self.tv.tv_input << 12).to_bytes(2, "big")),
                ("power", self.avr, self.avr.address.logical, 0x44, b"\x6d")):
            values = {"source": self.scan.initiators[component.gpio], "destination": destination,
                      "opcode": opcode, "operands": operands, "timestamp_ns": time.monotonic_ns()}
            transport = "wire"
            if changed and kind == changed[0]:
                values.update({key: value for key, value in changed[1].items() if key != "transport"})
                transport = changed[1].get("transport", transport)
            component.receive(Frame(**values), transport=transport)
        return self


class LookupNameGateTests(unittest.TestCase):
    def test_local_and_member_pending_original_queries_release_real_name_and_report(self):
        for member in (False, True):
            with self.subTest(member=member):
                f = GateFixture(member=member)
                with f.gate as gate:
                    f.missing().pending().b_progress()
                    self.assertIs(gate.wait_for_progress(since=f.action, timeout=.01), gate)
                    self.assertFalse(any(frame.opcode == 0x47 for frame in f.transmitted))
                    self.assertIsNone(gate.released_at)
                    gate.release().release()
                    self.assertIsInstance(gate.released_at, int)
                    replies = [frame for frame in f.transmitted if frame.opcode == 0x47]
                    self.assertEqual(len(replies), 1)
                    self.assertEqual(replies[0].raw,
                        Frame(f.source.address.logical, f.scan.initiators[f.source.gpio], 0x47,
                              f.source.name.encode("ascii")).raw)
                    self.assertEqual([row["kind"] for row in gate._report["checks"]],
                                     ["physical_progress", "release"])
                    self.assertLessEqual(set(gate._report), _CONTROL_OBSERVATION_FIELDS)
                self.assertIsNone(f.source.hooks.before_tx)
                self.assertNotIn(gate._observe, f.source._wire_observers)

    def test_progress_accepts_b_before_or_after_a_queries_on_either_owner(self):
        for member in (False, True):
            for first in ("a", "b"):
                with self.subTest(member=member, first=first):
                    f = GateFixture(member=member)
                    with f.gate as gate:
                        f.missing()
                        if first == "a":
                            f.pending().b_progress()
                        else:
                            f.b_progress().pending()
                        if member:
                            f.status["activity_settling"] = False
                        with patch.object(f.f.dut, "get_system", wraps=f.f.dut.get_system) as system, \
                             patch.object(f.f.dut, "get_state", wraps=f.f.dut.get_state) as state:
                            self.assertIs(gate.wait_for_progress(since=f.action, timeout=.01), gate)
                            gate.release()
                        self.assertEqual((system.call_count, state.call_count), (1, 1))
                        check = gate._report["checks"][0]
                        self.assertEqual(check["kind"], "physical_progress")
                        self.assertEqual(len(check["progress_frames"]), 3)
                        self.assertTrue(all(row["received_monotonic_ns"]
                            <= check["state_request_started_monotonic_ns"] for row in check["progress_frames"]))
                        self.assertLessEqual(check["state_received_monotonic_ns"], gate.released_at)
                    for witness in (f.tv, f.avr):
                        self.assertNotIn(gate._observe_progress, witness._wire_observers)

    def test_progress_rejects_wrong_wire_sender_target_opcode_input_and_virtual_or_stale_frames(self):
        variants = [("wake", dict(source=14)), ("wake", dict(destination=15)),
                    ("wake", dict(opcode=0x36)), ("wake", dict(timestamp_ns=1)),
                    ("route", dict(operands=b"\x30\0")), ("route", dict(destination=0)),
                    ("route", dict(transport="virtual")), ("power", dict(operands=b"\x6c")),
                    ("power", dict(destination=15)), ("power", dict(transport="virtual"))]
        for changed in variants:
            with self.subTest(changed=changed):
                f = GateFixture()
                with f.gate as gate:
                    f.missing().pending().b_progress(changed=changed)
                    with self.assertRaises(TimeoutError):
                        gate.wait_for_progress(since=f.action, timeout=.002)

    def test_progress_cannot_pass_after_scan_ends_or_name_returns_or_owner_changes(self):
        for change in ("generation", "finished", "name", "owner"):
            with self.subTest(change=change):
                f = GateFixture()
                with f.gate as gate:
                    f.missing().pending().b_progress()
                    if change == "generation":
                        f.status["discovery_generation"] += 1
                    elif change == "finished":
                        f.status["discovering"] = False
                    elif change == "name":
                        f.status["devices"].append({"name": next(iter(gate._selectors))})
                    else:
                        f.f.source.system["membership"]["peers"][0]["boot_id"] = "e" * 32
                    with self.assertRaises(AssertionError):
                        gate.wait_for_progress(since=f.action, timeout=.01)

    def test_only_name_responses_are_dropped_and_hooks_do_not_wait(self):
        f = GateFixture()
        with f.gate:
            f.missing()
            with patch.object(f.gate._condition, "wait", side_effect=AssertionError("blocked hook")):
                f.query(0x83)
                f.query(0x46)
                f.source.send(15, 0x87, b"\0\0\1")
            self.assertEqual([frame.opcode for frame in f.transmitted], [0x84, 0x87])

    def test_stale_virtual_wrong_sender_broadcast_or_malformed_queries_cannot_prove_lookup(self):
        variants = [dict(timestamp=1), dict(transport="virtual"), dict(source=14),
                    dict(destination=15), dict(operands=b"\0")]
        for changed in variants:
            with self.subTest(changed=changed):
                f = GateFixture()
                with f.gate as gate:
                    f.missing()
                    f.status.update(discovering=True, activity_settling=True)
                    f.b_progress()
                    f.query(0x83)
                    f.query(0x46, **changed)
                    with self.assertRaises(TimeoutError):
                        gate.wait_for_progress(since=f.action, timeout=.002)

    def test_physical_name_query_alone_is_not_enumeration(self):
        f = GateFixture()
        with f.gate as gate:
            f.missing()
            f.status.update(discovering=True, activity_settling=True)
            f.b_progress()
            f.query(0x46)
            with self.assertRaises(TimeoutError):
                gate.wait_for_progress(since=f.action, timeout=.002)

    def test_completed_scan_or_activity_cannot_claim_pending(self):
        for changed in (dict(discovering=False), dict(activity_settling=False),
                        dict(discovery_generation=99), dict(expected_boot_id="e" * 32),
                        dict(expected_config_revision="99"), dict(expected_presentation_revision="99")):
            with self.subTest(changed=changed):
                f = GateFixture()
                with f.gate as gate:
                    f.missing().pending().b_progress()
                    f.status.update(changed)
                    with self.assertRaises(AssertionError):
                        gate.wait_for_progress(since=f.action, timeout=.01)

    def test_original_action_scan_and_current_owner_proof_are_required(self):
        changes = [dict(scan=object()), dict(session=object()), dict(testbed=object()),
                   dict(owner_boots={}), dict(initiators={}), dict(kind="standby")]
        for changed in changes:
            with self.subTest(changed=changed):
                f = GateFixture(member=True)
                with f.gate as gate:
                    f.missing().pending().b_progress()
                    with self.assertRaises(AssertionError):
                        gate.wait_for_progress(since=replace(f.action, **changed), timeout=.01)

    def test_source_must_really_be_absent_and_remain_unresolved(self):
        f = GateFixture()
        original = deepcopy(f.f.source.system["observations"])
        with f.gate as gate:
            f.missing().pending().b_progress()
            f.f.source.system["observations"] = original
            with self.assertRaisesRegex(AssertionError, "still available"):
                gate.wait_for_progress(since=f.action, timeout=.01)

    def test_progress_rejects_interval_ending_after_owner_snapshot(self):
        f = GateFixture()
        with f.gate as gate:
            f.missing().pending().b_progress()
            original = f.f.dut.get_system

            def system():
                result = original()
                f.status["discovering"] = False
                return result

            f.f.dut.get_system = system
            with self.assertRaisesRegex(AssertionError, "ended before"):
                gate.wait_for_progress(since=f.action, timeout=.01)

    def test_preexisting_hooks_rejected_and_removed_observer_invalidates_proof(self):
        f = GateFixture()
        f.source.hooks = Hooks(before_rx=lambda component, frame: frame)
        with self.assertRaises(ValueError):
            f.gate.__enter__()
        self.assertIsNotNone(f.source.hooks.before_rx)
        f = GateFixture()
        with f.gate as gate:
            f.missing().pending().b_progress()
            f.source._wire_observers.remove(gate._observe)
            with self.assertRaisesRegex(AssertionError, "original wire observation"):
                gate.wait_for_progress(since=f.action, timeout=.01)

    def test_release_failure_restores_hook_and_preserves_primary_error(self):
        f = GateFixture()
        primary = RuntimeError("original failure")
        with self.assertRaisesRegex(RuntimeError, "original failure"):
            with f.gate:
                f.missing().pending()
                f.f.session.transmit = lambda component, frame: TxResult(frame, 0, time.monotonic_ns())
                raise primary
        self.assertIsNone(f.source.hooks.before_tx)
        self.assertNotIn(f.gate._observe, f.source._wire_observers)
        self.assertIn("Lookup name gate release failed: AssertionError", primary.__notes__)


if __name__ == "__main__":
    unittest.main()
