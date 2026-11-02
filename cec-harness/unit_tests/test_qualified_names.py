"""Qualified-selection scenario coverage and physical negative evidence."""

from contextlib import ExitStack
from pathlib import Path
from types import SimpleNamespace
import sys
import threading
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import Component, DUT, Hooks, Key, PowerState, TestBed, TvComponent
from amity_harness.cec import Frame
from amity_harness.qualified_names import observe_no_control
from functional_tests import qualified_names
from unit_tests.test_consortium_scenarios import Scenario


class QualifiedNamesScenario(Scenario):
    def __init__(self, *, wrong_key=False, forbidden_control=False):
        super().__init__()
        self.wrong_key, self.forbidden_control = wrong_key, forbidden_control
        self.observations = []

    def observe(self, scan, *components):
        fixture = self

        class Observation:
            def __enter__(self):
                fixture.observations.append(tuple(components))
                fixture.events.append(("observe.start", components[0].dut.name))
                self.checked = False
                return self

            def assert_clear(self):
                if fixture.forbidden_control:
                    raise AssertionError("Wrong-owner control was observed")
                self.checked = True
                fixture.events.append(("observe.clear", components[0].dut.name))

            def __exit__(self, exc_type, exc, traceback):
                if exc_type is None and not self.checked:
                    raise AssertionError("Negative physical evidence must be asserted")
                fixture.events.append(("observe.close", components[0].dut.name))

        return Observation()

    def key(self, main, key):
        fixture = self
        selected_owner = next(row[1] for row in reversed(self.events) if row[0] == "activity.start")

        class Action:
            def assert_received_by(self, target):
                if fixture.wrong_key or target.dut.name != selected_owner:
                    raise AssertionError("The key was sent to the wrong physical recipient")
                fixture.events.append(("key", key, target.dut.name, target.name))
                return self

        return Action()

    def run(self):
        with ExitStack() as patches:
            patches.enter_context(patch.object(qualified_names, "TestBed", self.bed))
            patches.enter_context(patch.object(qualified_names, "observe_no_control", self.observe))
            patches.enter_context(patch.object(DUT, "add_amity", lambda main, candidate: self.add(main, candidate)))
            patches.enter_context(patch.object(DUT, "scan_hdmi", lambda main: self.scan(main)))
            patches.enter_context(patch.object(DUT, "create_activity", lambda main, name, **kw: self.activity(main, name, **kw)))
            patches.enter_context(patch.object(DUT, "press_key", lambda main, key: self.key(main, key)))
            patches.enter_context(patch.object(Component, "assert_power", lambda component, power: self.assert_power(component, power)))
            patches.enter_context(patch.object(Component, "wait_for_power", lambda component, power, **kw: self.wait_power(component, power, **kw)))
            patches.enter_context(patch.object(Component, "wait_for_input", lambda avr, player, **kw: self.route(avr, player, **kw)))
            qualified_names.test_qualified_names()


class QualifiedNamesScenarioTests(unittest.TestCase):
    def test_same_labels_addresses_and_physical_addresses_exercise_both_owners(self):
        fixture = QualifiedNamesScenario()
        fixture.run()
        main, member = fixture.duts
        first = tuple(component for component in fixture.components if component.dut is main)
        second = tuple(component for component in fixture.components if component.dut is member)
        self.assertEqual([(c.name, c.address.logical, c.address.physical) for c in first],
                         [(c.name, c.address.logical, c.address.physical) for c in second])
        self.assertEqual([c.name for c in first], ["Same TV", "Same AVR", "Same Player"])
        self.assertEqual(len({c.gpio for c in fixture.components}), 4)
        self.assertEqual(fixture.observations, [second, first])
        for kind in ("scan.contains", "scan.only"):
            self.assertEqual([row[1] for row in fixture.events if row[0] == kind], [fixture.components])
        self.assertEqual(len([row for row in fixture.events if row[0] == "scan.on"]), 6)
        self.assertEqual([row[1:] for row in fixture.events if row[0] == "key"],
                         [(Key.VOLUME_UP, "amity-dut", "Same AVR"),
                          (Key.RIGHT, "amity-dut", "Same Player"),
                          (Key.VOLUME_UP, "amity-dut-2", "Same AVR"),
                          (Key.RIGHT, "amity-dut-2", "Same Player")])
        for owner in (main.name, member.name):
            for kind in ("start", "standby"):
                self.assertEqual(sum(row == ("power.wait", kind, owner, name)
                                     for row in fixture.events for name in ("Same TV", "Same AVR", "Same Player")), 3)
        self.assertEqual([row[1] for row in fixture.events if row[0] == "observe.clear"],
                         [member.name, main.name])
        self.assertEqual(fixture.events[-1], ("bed.close",))

    def test_wrong_owner_key_and_control_fail_and_cleanup(self):
        for kwargs, message in (({"wrong_key": True}, "wrong physical recipient"),
                                ({"forbidden_control": True}, "Wrong-owner control")):
            with self.subTest(**kwargs):
                fixture = QualifiedNamesScenario(**kwargs)
                with self.assertRaisesRegex(AssertionError, message):
                    fixture.run()
                self.assertFalse(fixture.members)
                self.assertFalse(fixture.activities)
                self.assertEqual(fixture.events[-1], ("bed.close",))


class NegativeControlEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.dut = DUT("amity-dut")
        self.component = TvComponent(self.dut.tentacle(0).front, "Same TV",
                                     hooks=Hooks(before_rx=lambda component, frame: None))
        self.component.address = SimpleNamespace(logical=0)
        self.records = []
        self.session = SimpleNamespace(_lock=threading.RLock(), _stop=threading.Event(),
                                       _wire_idle_since_ns={self.component.gpio: 10**30},
                                       assert_healthy=lambda: None,
                                       _record=lambda *args, **kw: self.records.append((args, kw)))
        self.bed = SimpleNamespace(session=self.session, _entered=True, _closed=False)
        self.dut._testbed = self.bed
        self.component._session = self.session
        self.scan = SimpleNamespace(dut=self.dut, expected=(self.component,), _session=self.session,
                                    _testbed=self.bed, initiators={self.component.gpio: 4},
                                    validate_owners=lambda components: None)
        self.dut.last_scan = self.scan
        self.component._wire_context = lambda since: (self.session, since, 4, None)
        self.ticks = 1_000_000
        self.clock = patch("amity_harness.qualified_names.time.monotonic_ns", self.now)
        self.clock.start()
        self.addCleanup(self.clock.stop)

    def now(self):
        self.ticks += 10_000
        return self.ticks

    def receive(self, opcode, *, source=4, transport="wire", timestamp=None):
        self.component.receive(Frame(source, 0, opcode, timestamp_ns=self.now() if timestamp is None else timestamp),
                               transport=transport)

    def test_control_on_inactive_wire_fails_even_when_responder_hook_drops_it(self):
        for opcode in (0x04, 0x0d, 0x36, 0x44, 0x45, 0x80, 0x81, 0x82, 0x86):
            with self.subTest(opcode=opcode):
                with self.assertRaisesRegex(AssertionError, "inactive owner's wire"):
                    with observe_no_control(self.scan, self.component, duration=0.000001) as observation:
                        self.receive(opcode)
                        observation.assert_clear()
                self.assertFalse(self.component._wire_observers)

    def test_virtual_stale_and_peer_control_cannot_become_dut_wire_evidence(self):
        with observe_no_control(self.scan, self.component, duration=0.000001) as observation:
            self.receive(0x44, transport="virtual")
            self.receive(0x44, timestamp=1)
            self.receive(0x44, source=8)
            self.receive(0x83)
            observation.assert_clear()
        self.assertEqual(len(self.bed.control_observations), 1)
        proof = self.bed.control_observations[0]
        self.assertEqual(proof["kind"], "no_owner_control")
        self.assertEqual(proof["wires"], [self.component.gpio])
        self.assertGreaterEqual(proof["idle_receive_since_ns"][str(self.component.gpio)],
                                proof["observation_end_monotonic_ns"])

    def test_missing_wire_clock_cannot_pass(self):
        with self.assertRaisesRegex(AssertionError, "without a wire timestamp"):
            with observe_no_control(self.scan, self.component, duration=0.000001) as observation:
                self.receive(0x83, timestamp=0)
                observation.assert_clear()

    def test_missing_receive_coverage_and_changed_owner_proof_fail(self):
        self.session._wire_idle_since_ns.clear()
        with self.assertRaisesRegex(TimeoutError, "did not drain"):
            with observe_no_control(self.scan, self.component, duration=0.000001) as observation:
                observation.assert_clear()
        self.session._wire_idle_since_ns[self.component.gpio] = 10**30
        with self.assertRaisesRegex(AssertionError, "different active TestBed"):
            with observe_no_control(self.scan, self.component, duration=0.000001) as observation:
                self.dut.last_scan = None
                observation.assert_clear()

    def test_unasserted_observation_fails_but_does_not_hide_an_existing_failure(self):
        with self.assertRaisesRegex(RuntimeError, "not asserted"):
            with observe_no_control(self.scan, self.component, duration=0.000001):
                pass
        with self.assertRaisesRegex(ValueError, "original failure"):
            with observe_no_control(self.scan, self.component, duration=0.000001):
                raise ValueError("original failure")
        self.assertFalse(self.component._wire_observers)

    def test_report_retains_bounded_negative_proof_without_arbitrary_fields(self):
        dut = DUT("amity-dut")
        bed = TestBed(dut)
        record = {"kind": "no_owner_control", "owner_ids": [dut.identity.device_id],
                  "wires": [23], "since_monotonic_ns": 100, "observation_end_monotonic_ns": 200,
                  "idle_receive_since_ns": {"23": 201}, "observed_frame_count": 4,
                  "password": "must not appear"}
        bed.control_observations = [record]
        proof, = bed.report["control_observations"]
        self.assertNotIn("password", proof)
        self.assertEqual(proof["idle_receive_since_ns"], {"23": 201})
        record["owner_ids"].append("later mutation")
        self.assertEqual(proof["owner_ids"], [dut.identity.device_id])


if __name__ == "__main__":
    unittest.main()
