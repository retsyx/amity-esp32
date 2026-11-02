"""Handoff ordering uses original physical evidence across isolated wires."""

from contextlib import contextmanager
from pathlib import Path
from types import SimpleNamespace
import sys
import json
import threading
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import AvrComponent, CecSession, DUT, Frame, Hooks, PlaybackComponent, TestBed, TvComponent, TxResult
from amity_harness.transitions import ActivityHandoff, observe_no_standby


class IdleWire:
    def __init__(self, gpio):
        self.closed = threading.Event()

    def open(self):
        pass

    def configure(self, components):
        pass

    def transmit(self, frame):
        return TxResult(frame, 1, time.monotonic_ns())

    def receive(self, timeout):
        self.closed.wait(timeout)
        return None

    def close(self):
        self.closed.set()


class ActivityHandoffTests(unittest.TestCase):
    @contextmanager
    def fixture(self):
        dut = DUT("amity-dut-4")
        previous = TvComponent(dut.tentacle(1).front, "Old TV")
        selected = TvComponent(dut.tentacle(2).front, "New TV")
        avr = AvrComponent(dut.tentacle(1).back, "Shared AVR")
        player = PlaybackComponent(avr, "Shared Player", port=1)
        components = (previous, selected, avr, player)
        with CecSession(components, backend_factory=IdleWire) as session:
            initiators = {previous.gpio: 4, selected.gpio: 4, avr.gpio: 0}
            dut.last_scan = SimpleNamespace(initiators=initiators, _session=session)
            with ActivityHandoff(previous, selected, shared=(avr, player), receiver=avr, source=player) as observation:
                action = SimpleNamespace(dut=dut, session=session, started_ns=time.monotonic_ns(), initiators=initiators)
                yield observation, action, previous, selected, avr, player, session

    def receive(self, component, action, opcode, operands=b"", *, timestamp=None,
                destination=None, source=None, transport="wire"):
        frame = Frame(action.initiators[component.gpio] if source is None else source,
                      component.address.logical if destination is None else destination,
                      opcode, operands, timestamp_ns=time.monotonic_ns() if timestamp is None else timestamp)
        component.receive(frame, transport=transport)

    def commands(self, fixture, *, order=(0, 1, 2), transport="wire", timestamp=None):
        observation, action, previous, selected, avr, player, session = fixture
        times = [time.monotonic_ns() for _ in range(3)] if timestamp is None else [timestamp] * 3
        self.receive(previous, action, 0x36, timestamp=times[order[0]], transport=transport)
        self.receive(selected, action, 0x04, timestamp=times[order[1]], transport=transport)
        self.receive(avr, action, 0x86, player.address.physical.to_bytes(2, "big"),
                     destination=15, timestamp=times[order[2]], transport=transport)
        # This unit fixture has no queued backend frames. Real runs must wait
        # for each worker to complete an idle receive through the boundary.
        session._wire_idle_since_ns = {gpio: time.monotonic_ns() + 10**12 for gpio in action.initiators}

    def test_ordered_physical_commands_are_retained_in_report_evidence(self):
        with self.fixture() as fixture:
            self.commands(fixture)
            observation, action, _, _, avr, _, _ = fixture
            self.assertIs(observation.assert_ordered(since=action), observation)
            proof = avr.routing_assertions[-1]
            self.assertEqual(proof["kind"], "wire_handoff")
            self.assertEqual(proof["timestamp_kind"], "frame_completion")
            self.assertLess(proof["standby_wire_timestamp_ns"], proof["power_on_wire_timestamp_ns"])
            self.assertLess(proof["power_on_wire_timestamp_ns"], proof["routing_wire_timestamp_ns"])

    def test_testbed_report_retains_handoff_order_clock_and_drain_evidence_without_secrets(self):
        with self.fixture() as fixture:
            self.commands(fixture)
            observation, action, _, _, avr, _, _ = fixture
            observation.assert_ordered(since=action)
            avr._routing_checks[-1]["private_token"] = "PRIVATE_HANDOFF_TOKEN"
            report = TestBed(*fixture[2:6]).report
            row = next(component for component in report["components"] if component["name"] == avr.name)
            proof = row["routing_assertions"][-1]
            for field in ("timestamp_kind", "previous", "selected", "shared", "standby_wire_timestamp_ns",
                          "power_on_wire_timestamp_ns", "routing_wire_timestamp_ns", "standby_completion_upper_ns",
                          "power_on_completion_upper_ns", "observation_end_monotonic_ns", "observed_wires",
                          "idle_receive_since_ns"):
                self.assertEqual(proof[field], avr.routing_assertions[-1][field])
            self.assertNotIn("PRIVATE_HANDOFF_TOKEN", json.dumps(report))

    def test_power_before_standby_and_route_before_power_fail(self):
        for order in ((1, 0, 2), (0, 2, 1)):
            with self.subTest(order=order), self.assertRaisesRegex(AssertionError, "out of order"):
                with self.fixture() as fixture:
                    self.commands(fixture, order=order)
                    fixture[0].assert_ordered(since=fixture[1])

    def test_transient_shared_standby_then_power_on_cannot_hide_behind_final_state(self):
        for index in (4, 5):
            with self.subTest(shared=index), self.assertRaisesRegex(AssertionError, "Shared equipment"):
                with self.fixture() as fixture:
                    self.commands(fixture)
                    component = fixture[index]
                    self.receive(component, fixture[1], 0x36)
                    self.receive(component, fixture[1], 0x44, b"\x6d")
                    fixture[0].assert_ordered(since=fixture[1])

    def test_dropped_or_rewritten_shared_standby_still_fails(self):
        for rewrite in (False, True):
            with self.subTest(rewrite=rewrite), self.assertRaisesRegex(AssertionError, "Shared equipment"):
                with self.fixture() as fixture:
                    self.commands(fixture)
                    avr = fixture[4]
                    avr.hooks = Hooks(before_rx=lambda component, frame: Frame(frame.source, frame.destination, 0x8f)
                                     if rewrite else None)
                    self.receive(avr, fixture[1], 0x36)
                    fixture[0].assert_ordered(since=fixture[1])

    def test_virtual_and_stale_frames_cannot_establish_order(self):
        for kind in ("virtual", "stale"):
            with self.subTest(kind=kind), self.assertRaisesRegex(AssertionError, "lacks fresh physical"):
                with self.fixture() as fixture:
                    self.commands(fixture, transport="virtual" if kind == "virtual" else "wire",
                                  timestamp=fixture[1].started_ns - 1 if kind == "stale" else None)
                    fixture[0].assert_ordered(since=fixture[1])

    def test_wrong_initiator_cannot_establish_order(self):
        with self.assertRaisesRegex(AssertionError, "lacks fresh physical"):
            with self.fixture() as fixture:
                observation, action, previous, selected, avr, player, session = fixture
                self.receive(previous, action, 0x36, source=8)
                self.receive(selected, action, 0x04)
                self.receive(avr, action, 0x86, player.address.physical.to_bytes(2, "big"), destination=15)
                session._wire_idle_since_ns = {gpio: time.monotonic_ns() + 10**12 for gpio in action.initiators}
                observation.assert_ordered(since=action)

    def test_overlapping_ssh_clock_intervals_cannot_establish_order(self):
        with self.assertRaisesRegex(AssertionError, "out of order"):
            with self.fixture() as fixture:
                self.commands(fixture)
                fixture[6]._backends[fixture[2].gpio].clock_uncertainty_ns = 10**9
                fixture[0].assert_ordered(since=fixture[1])

    def test_undrained_wire_cannot_prove_absence(self):
        with self.assertRaisesRegex(TimeoutError, "did not drain"):
            with self.fixture() as fixture:
                self.commands(fixture)
                fixture[4]._power_timeout = 0.005
                fixture[6]._wire_idle_since_ns = {}
                fixture[0].assert_ordered(since=fixture[1])

    def test_observers_are_removed_after_failure(self):
        components = ()
        with self.assertRaisesRegex(AssertionError, "out of order"):
            with self.fixture() as fixture:
                components = fixture[2:6]
                self.commands(fixture, order=(1, 0, 2))
                fixture[0].assert_ordered(since=fixture[1])
        self.assertTrue(all(not component._wire_observers for component in components))

    def test_repair_observation_allows_power_on_and_retains_absence_proof(self):
        with self.fixture() as fixture:
            self.commands(fixture)
            fixture[0].assert_ordered(since=fixture[1])
            previous, selected, avr, player = fixture[2:6]
            with observe_no_standby(previous, avr, player) as observation:
                action = SimpleNamespace(**{**vars(fixture[1]), "started_ns": time.monotonic_ns()})
                for component, opcode, operands in ((previous, 0x04, b""), (avr, 0x44, b"\x6d"),
                                                   (player, 0x44, b"\x6d")):
                    self.receive(component, action, opcode, operands)
                observation.assert_preserved(since=action)
            self.assertTrue(all(component.power_assertions[-1]["kind"] == "wire_no_standby"
                                for component in (previous, avr, player)))
            previous._power_checks[-1]["private_token"] = "PRIVATE_REPAIR_TOKEN"
            report = TestBed(previous, avr, player).report
            for row in report["components"]:
                proof = row["power_assertions"][-1]
                self.assertEqual(proof["kind"], "wire_no_standby")
                self.assertEqual(set(proof["observed_wires"]), set(action.initiators) - {selected.gpio})
                self.assertIn("idle_receive_since_ns", proof)
                self.assertIn("observation_end_monotonic_ns", proof)
            self.assertNotIn("PRIVATE_REPAIR_TOKEN", json.dumps(report))

    def test_repair_observation_rejects_standby_even_when_hook_drops_it(self):
        with self.assertRaisesRegex(AssertionError, "Standby during activity repair"):
            with self.fixture() as fixture:
                self.commands(fixture)
                fixture[0].assert_ordered(since=fixture[1])
                avr = fixture[4]
                with observe_no_standby(avr) as observation:
                    action = SimpleNamespace(**{**vars(fixture[1]), "started_ns": time.monotonic_ns()})
                    avr.hooks = Hooks(before_rx=lambda component, frame: None)
                    self.receive(avr, action, 0x36)
                    observation.assert_preserved(since=action)


if __name__ == "__main__":
    unittest.main()
