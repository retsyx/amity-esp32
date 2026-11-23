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

from amity_harness import (AvrComponent, CecSession, DUT, Frame, Hooks, PlaybackComponent, PowerState, TestBed,
                           TvComponent, TxResult)
from amity_harness.transitions import ActivityDisplayStandby, ActivityHandoff, observe_no_standby, observe_no_wake
from unit_tests.virtual_time import use_virtual_time


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
    def setUp(self):
        self.clock = use_virtual_time(self)

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
        # Consecutive host-clock reads may be equal at its actual resolution.
        # Give the simulated completions explicit distinct instants instead.
        start = time.monotonic_ns()
        times = [start + index for index in range(3)] if timestamp is None else [timestamp] * 3
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
                          "power_on_wire_timestamp_ns", "routing_wire_timestamp_ns", "observation_end_monotonic_ns", "observed_wires",
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

    def test_source_announcement_observes_full_window_and_retains_physical_trigger(self):
        with self.fixture() as fixture:
            self.commands(fixture)
            fixture[0].assert_ordered(since=fixture[1])
            previous, _, avr, player = fixture[2:6]
            with observe_no_standby(previous, avr, player) as observation:
                announcement = player.activate()
                observation.assert_preserved_after(announcement, within=0.010)
                self.assertGreaterEqual(time.monotonic_ns(), announcement.timestamp_ns + 10_000_000)
            report = TestBed(previous, avr, player).report
            for row in report["components"]:
                proof = row["power_assertions"][-1]
                self.assertEqual(proof["kind"], "wire_no_standby")
                self.assertEqual(proof["trigger_frame"], announcement.frame.raw.hex(":"))
                self.assertEqual(proof["trigger_wire_timestamp_ns"], announcement.timestamp_ns)
                self.assertEqual(proof["max_elapsed_ns"], 10_000_000)
                self.assertGreaterEqual(proof["observation_end_monotonic_ns"], announcement.timestamp_ns + 10_000_000)

    def test_source_announcement_rejects_transient_or_hook_hidden_power_down(self):
        for opcode, operands, hook in ((0x36, b"", None), (0x44, b"\x6c", None),
                                       (0x44, b"\x40", None), (0x36, b"", lambda c, f: None),
                                       (0x36, b"", lambda c, f: Frame(f.source, f.destination, 0x8f))):
            with self.subTest(opcode=opcode, operands=operands, hook=hook):
                with self.assertRaisesRegex(AssertionError, "Standby during source announcement"):
                    with self.fixture() as fixture:
                        self.commands(fixture)
                        fixture[0].assert_ordered(since=fixture[1])
                        previous, _, avr, player = fixture[2:6]
                        with observe_no_standby(previous, avr, player) as observation:
                            announcement = player.activate()
                            avr.hooks = Hooks(before_rx=hook)
                            self.receive(avr, fixture[1], opcode, operands)
                            avr.hooks = Hooks()
                            self.receive(avr, fixture[1], 0x44, b"\x6d")
                            observation.assert_preserved_after(announcement, within=0.001)
                self.assertFalse(any(component._wire_observers for component in (previous, avr, player)))

    def test_source_announcement_catches_power_down_arriving_during_window(self):
        with self.assertRaisesRegex(AssertionError, "Standby during source announcement"):
            with self.fixture() as fixture:
                self.commands(fixture)
                fixture[0].assert_ordered(since=fixture[1])
                previous, _, avr, player = fixture[2:6]
                with observe_no_standby(previous, avr, player) as observation:
                    announcement = player.activate()
                    timer = threading.Timer(0.010, self.receive, args=(previous, fixture[1], 0x36),
                                            kwargs={"destination": 15})
                    timer.start()
                    try:
                        observation.assert_preserved_after(announcement, within=0.030)
                    finally:
                        timer.join()

    def test_virtual_stale_and_other_initiator_power_down_cannot_spoil_announcement(self):
        with self.fixture() as fixture:
            self.commands(fixture)
            fixture[0].assert_ordered(since=fixture[1])
            previous, _, avr, player = fixture[2:6]
            with observe_no_standby(previous, avr, player) as observation:
                announcement = player.activate()
                self.receive(avr, fixture[1], 0x36, transport="virtual")
                self.receive(avr, fixture[1], 0x36, timestamp=announcement.timestamp_ns - 1)
                self.receive(avr, fixture[1], 0x36, source=8)
                observation.assert_preserved_after(announcement, within=0.001)

    def test_no_standby_requires_observed_fresh_successful_physical_announcement(self):
        for wrong in ("failed", "virtual", "opcode", "unobserved", "stale"):
            with self.subTest(wrong=wrong), self.assertRaises(AssertionError):
                with self.fixture() as fixture:
                    self.commands(fixture)
                    fixture[0].assert_ordered(since=fixture[1])
                    previous, _, avr, player = fixture[2:6]
                    before = player.activate() if wrong == "stale" else None
                    with observe_no_standby(previous, avr, *(() if wrong == "unobserved" else (player,))) as observation:
                        announcement = before or player.activate()
                        if wrong == "failed":
                            announcement = TxResult(announcement.frame, 0, announcement.timestamp_ns)
                        elif wrong == "virtual":
                            announcement = TxResult(announcement.frame, 1, announcement.timestamp_ns, "virtual")
                        elif wrong == "opcode":
                            announcement = TxResult(Frame(player.address.logical, 15, 0x84), 1,
                                                    announcement.timestamp_ns)
                        observation.assert_preserved_after(announcement, within=0.001)


class ActivityDisplayStandbyTests(unittest.TestCase):
    def setUp(self):
        self.clock = use_virtual_time(self)

    @contextmanager
    def fixture(self):
        dut = DUT("amity-dut")
        tv = TvComponent(dut.tentacle(0).front, "Previous TV")
        avr = AvrComponent(dut.tentacle(0).back, "Receiver")
        with CecSession((tv, avr), backend_factory=IdleWire) as session:
            initiators = {tv.gpio: 4, avr.gpio: 0}
            dut.last_scan = SimpleNamespace(initiators=initiators, _session=session)
            with ActivityDisplayStandby(tv) as observation:
                action = SimpleNamespace(dut=dut, session=session, started_ns=time.monotonic_ns(),
                                         initiators=initiators)
                yield observation, action, tv, avr, session

    def receive(self, tv, opcode=0x36, operands=b"", *, destination=None, source=4,
                timestamp=None, transport="wire"):
        tv.receive(Frame(source, tv.address.logical if destination is None else destination, opcode, operands,
                         timestamp_ns=time.monotonic_ns() if timestamp is None else timestamp), transport=transport)

    def drained(self, tv, session):
        session._wire_idle_since_ns[tv.gpio] = time.monotonic_ns() + 10**12

    def test_directed_standby_retries_and_metadata_retain_private_safe_drain_proof(self):
        with self.fixture() as (observation, action, tv, avr, session):
            self.receive(tv)
            self.receive(tv)
            self.receive(tv, 0x8f)
            self.receive(tv, 0x83)
            self.receive(tv, 0x90, b"\x01")
            self.drained(tv, session)
            self.assertIs(observation.assert_standby_only(since=action), observation)
            tv._power_checks[-1]["private_token"] = "PRIVATE_DISPLAY_TOKEN"
            report = TestBed(action.dut, tv, avr).report
            proof = next(row for row in report["components"] if row["name"] == tv.name)["power_assertions"][-1]
            self.assertEqual(proof["kind"], "wire_display_standby_only")
            self.assertEqual(proof["frame"], "40:36")
            self.assertEqual(proof["observed_wires"], [tv.gpio])
            self.assertEqual(proof["since_monotonic_ns"], action.started_ns)
            self.assertGreaterEqual(proof["idle_receive_since_ns"][str(tv.gpio)], proof["observation_end_monotonic_ns"])
            self.assertNotIn("PRIVATE_DISPLAY_TOKEN", json.dumps(report))
        self.assertFalse(tv._wire_observers)

    def test_missing_stale_virtual_wrong_sender_and_broadcast_cannot_prove_standby(self):
        for fault in ("missing", "stale", "virtual", "sender", "broadcast"):
            with self.subTest(fault=fault), self.assertRaises(AssertionError):
                with self.fixture() as (observation, action, tv, avr, session):
                    if fault != "missing":
                        self.receive(tv, timestamp=action.started_ns - 1 if fault == "stale" else None,
                                     transport="virtual" if fault == "virtual" else "wire",
                                     source=8 if fault == "sender" else 4,
                                     destination=15 if fault == "broadcast" else None)
                    self.drained(tv, session)
                    observation.assert_standby_only(since=action)

    def test_wake_key_and_routing_controls_cannot_hide_behind_hooks_or_final_standby(self):
        controls = ((0x04, b""), (0x0d, b""), (0x36, b"\x00"), (0x44, b"\x41"), (0x45, b""),
                    (0x80, b"\x10\x00\x20\x00"), (0x81, b"\x10\x00"), (0x82, b"\x10\x00"),
                    (0x86, b"\x10\x00"), (0x9d, b"\x10\x00"))
        for opcode, operands in controls:
            for hook in (None, "drop", "rewrite"):
                with self.subTest(opcode=opcode, hook=hook), self.assertRaisesRegex(AssertionError, "beyond directed Standby"):
                    with self.fixture() as (observation, action, tv, avr, session):
                        self.receive(tv)
                        if hook:
                            tv.hooks = Hooks(before_rx=lambda component, frame: None if hook == "drop" else
                                             Frame(frame.source, frame.destination, 0x8f, timestamp_ns=frame.timestamp_ns))
                        self.receive(tv, opcode, operands, destination=15 if opcode in (0x80, 0x81, 0x82, 0x86, 0x9d) else None)
                        tv.standby()
                        self.drained(tv, session)
                        observation.assert_standby_only(since=action)
                self.assertFalse(tv._wire_observers)

    def test_rewritten_standby_cannot_turn_other_wire_control_into_allowed_standby(self):
        with self.assertRaisesRegex(AssertionError, "beyond directed Standby"):
            with self.fixture() as (observation, action, tv, avr, session):
                tv.hooks = Hooks(before_rx=lambda component, frame: Frame(4, tv.address.logical, 0x36,
                                                                         timestamp_ns=frame.timestamp_ns))
                self.receive(tv, 0x82, b"\x10\x00", destination=15)
                self.drained(tv, session)
                observation.assert_standby_only(since=action)

    def test_undrained_or_changed_session_and_predating_action_fail_closed(self):
        for fault in ("coverage", "session", "stopped", "action"):
            expected = TimeoutError if fault == "coverage" else AssertionError
            with self.subTest(fault=fault), self.assertRaises(expected):
                with self.fixture() as (observation, action, tv, avr, session):
                    self.receive(tv)
                    self.drained(tv, session)
                    if fault == "coverage":
                        tv._power_timeout = 0.005
                        session._wire_idle_since_ns = {}
                    elif fault == "session":
                        action.session = object()
                    elif fault == "stopped":
                        session._stop.set()
                    else:
                        action.started_ns = observation._started - 1
                    observation.assert_standby_only(since=action)

    def test_only_one_front_tv_is_accepted(self):
        dut = DUT("amity-dut")
        for component in (AvrComponent(dut.tentacle(0).back, "Receiver"),
                          TvComponent(dut.tentacle(0).back, "Back TV"), None, ()):
            with self.subTest(component=component), self.assertRaises(ValueError):
                ActivityDisplayStandby(component)


class NoWakeTests(unittest.TestCase):
    """A thief during global standby: its Standby, then no wake-up for the hold."""

    hold = 3
    hold_ns = 3_000_000_000
    # Image/Text View On and the power keys, which count for all equipment.
    power_on = ((0x04, b""), (0x0d, b""), (0x44, b"\x40"), (0x44, b"\x6b"), (0x44, b"\x6d"))
    # What the equipment models also obey as a wake-up: (component, opcode,
    # operands, broadcast), where None operands are the player's physical address.
    model_wakes = (("avr", 0x70, None, False), ("player", 0x41, b"\x24", False), ("player", 0x44, b"\x44", False),
                   ("player", 0x44, b"\x60", False), ("player", 0x86, None, True))

    def setUp(self):
        self.clock = use_virtual_time(self)

    @contextmanager
    def fixture(self, observed=("tv", "avr", "player")):
        dut = DUT("amity-dut-4")
        tv = TvComponent(dut.tentacle(0).front, "Sleeping TV")
        avr = AvrComponent(dut.tentacle(0).back, "Sleeping AVR")
        player = PlaybackComponent(avr, "Former Source", port=1)
        other = TvComponent(dut.tentacle(1).front, "Unobserved TV")
        with CecSession((tv, avr, player, other), backend_factory=IdleWire) as session:
            initiators = {tv.gpio: 4, avr.gpio: 0, other.gpio: 4}
            dut.last_scan = SimpleNamespace(initiators=initiators, _session=session)
            f = SimpleNamespace(tv=tv, avr=avr, player=player, other=other, session=session, initiators=initiators)
            f.observation = observe_no_wake(*(getattr(f, name) for name in observed))
            with f.observation:
                yield f

    def wire(self, f, component, opcode, operands=b"", *, destination=None, source=None, timestamp=None):
        """Deliver one received frame to every component on a wire, as the session worker does."""
        frame = Frame(f.initiators[component.gpio] if source is None else source,
                      component.address.logical if destination is None else destination, opcode, operands,
                      timestamp_ns=time.monotonic_ns() if timestamp is None else timestamp)
        with f.session._lock:
            for peer in f.session.components:
                if peer.gpio == component.gpio:
                    peer.receive(frame)
        return frame

    def thief(self, f, *before):
        """The former source announces; Amity broadcasts Standby on both wires.

        ``before`` callables deliver frames right after the announcement. As
        on the bench, the front Standby follows about 60 ms later and the
        source's Standby about 120 ms after the announcement.
        """
        announcement = f.player.activate()
        self.assertTrue(announcement.ok)
        for deliver in before:
            deliver()
        time.sleep(0.06)
        self.wire(f, f.tv, 0x36, destination=15)
        time.sleep(0.06)
        standby = self.wire(f, f.player, 0x36, destination=15)
        f.player.wait_for_power(PowerState.STANDBY, since=announcement.timestamp_ns, command="standby", broadcast=True)
        return announcement, standby, f.player.power_assertions[-1]

    def check(self, f, announcement, proof, *, at=None, deliver=None):
        """Run the hold check while another thread calls ``deliver`` at ``at``."""
        delivered = []
        timer = None
        if at is not None:
            timer = threading.Timer(max(0, at - time.monotonic_ns()) / 1e9, lambda: delivered.append(deliver()))
            timer.start()
        try:
            f.observation.assert_no_wake_after(announcement, standby=proof, hold=self.hold)
        finally:
            if timer is not None:
                timer.join()
        return delivered

    def wakes(self):
        """Every waking command as (component, opcode, operands, broadcast)."""
        return [(name, opcode, operands, False) for name in ("tv", "avr", "player")
                for opcode, operands in self.power_on] + list(self.model_wakes)

    def test_standby_alone_passes_after_the_hold_and_retains_drain_proof(self):
        with self.fixture() as f:
            announcement, standby, proof = self.thief(f)
            # wait_for_power leaves the proof of the Standby that answered the thief last.
            self.assertEqual({key: proof[key] for key in ("kind", "command", "frame", "destination", "wire_timestamp_ns")},
                             {"kind": "wire_command", "command": "standby", "frame": "0f:36", "destination": 15,
                              "wire_timestamp_ns": standby.timestamp_ns})
            self.assertGreaterEqual(standby.timestamp_ns - announcement.timestamp_ns, 120_000_000)
            for component in (f.tv, f.avr, f.player):
                self.wire(f, component, 0x36)
                self.wire(f, component, 0x8f)
            self.wire(f, f.avr, 0x36, destination=15)
            self.assertIs(f.observation.assert_no_wake_after(announcement, standby=proof, hold=self.hold), f.observation)
            end = standby.timestamp_ns + self.hold_ns
            self.assertGreaterEqual(time.monotonic_ns(), end)
            for component in (f.tv, f.avr, f.player):
                row = component.power_assertions[-1]
                self.assertEqual(row["kind"], "wire_no_wake")
                self.assertEqual((row["since_monotonic_ns"], row["trigger_wire_timestamp_ns"]),
                                 (announcement.timestamp_ns, announcement.timestamp_ns))
                self.assertEqual(row["trigger_frame"], announcement.frame.raw.hex(":"))
                self.assertEqual((row["standby_wire_timestamp_ns"], row["hold_ns"]), (standby.timestamp_ns, self.hold_ns))
                self.assertGreaterEqual(row["observation_end_monotonic_ns"], end)
                self.assertEqual(row["observed_wires"], sorted({f.tv.gpio, f.avr.gpio}))
                for gpio in row["observed_wires"]:
                    self.assertGreaterEqual(row["idle_receive_since_ns"][str(gpio)], row["observation_end_monotonic_ns"])
            report = TestBed(f.tv, f.avr, f.player).report
            for row in report["components"]:
                reported = row["power_assertions"][-1]
                self.assertEqual(reported["kind"], "wire_no_wake")
                self.assertEqual((reported["standby_wire_timestamp_ns"], reported["hold_ns"]),
                                 (standby.timestamp_ns, self.hold_ns))
                self.assertGreaterEqual(reported["observation_end_monotonic_ns"], end)
            for component in (f.tv, f.avr, f.player):
                component.assert_power(PowerState.STANDBY)
        self.assertFalse(any(component._wire_observers for component in (f.tv, f.avr, f.player)))

    def test_each_wake_fails_before_the_sources_standby_and_inside_the_hold(self):
        for name, opcode, operands, broadcast in self.wakes():
            for phase in ("before", "hold"):
                with self.subTest(component=name, opcode=opcode, operands=operands, phase=phase):
                    with self.assertRaises(AssertionError) as raised:
                        with self.fixture() as f:
                            target = getattr(f, name)
                            payload = f.player.address.physical.to_bytes(2, "big") if operands is None else operands
                            frame = Frame(f.initiators[target.gpio], 15 if broadcast else target.address.logical,
                                          opcode, payload)
                            expected = f"{target.name} received waking command {frame.raw.hex(':')} "

                            def deliver():
                                return self.wire(f, target, opcode, payload, destination=frame.destination)

                            if phase == "before":
                                announcement, standby, proof = self.thief(f, deliver)
                                self.check(f, announcement, proof)
                            else:
                                announcement, standby, proof = self.thief(f)
                                self.check(f, announcement, proof, at=standby.timestamp_ns + self.hold_ns - 100_000_000,
                                           deliver=deliver)
                    self.assertIn(expected, str(raised.exception))
                    self.assertFalse(any(component._wire_observers for component in (f.tv, f.avr, f.player)))

    def test_wake_that_a_later_standby_reverses_fails_even_when_hidden(self):
        # The final state alone cannot show a wake-up that a later Standby
        # undoes, such as a player re-routed after its Standby.
        for name, opcode, operands, broadcast in (("player", 0x44, b"\x6d", False), ("player", 0x86, None, True)):
            for hidden in (False, True):
                with self.subTest(component=name, opcode=opcode, hidden=hidden):
                    with self.assertRaisesRegex(AssertionError, "received waking command"):
                        with self.fixture() as f:
                            target = getattr(f, name)
                            payload = f.player.address.physical.to_bytes(2, "big") if operands is None else operands
                            announcement, standby, proof = self.thief(f)
                            target.hooks = Hooks(before_rx=(lambda component, frame: None) if hidden else None)
                            self.wire(f, target, opcode, payload, destination=15 if broadcast else None)
                            self.assertEqual(target.power, PowerState.STANDBY if hidden else PowerState.ON)
                            target.hooks = Hooks()
                            self.wire(f, target, 0x36, destination=15)
                            target.assert_power(PowerState.STANDBY)
                            self.check(f, announcement, proof)

    def test_other_initiators_unobserved_wires_virtual_and_late_frames_are_ignored(self):
        with self.fixture() as f:
            announcement, standby, proof = self.thief(f)
            self.wire(f, f.tv, 0x04, source=8)
            self.wire(f, f.other, 0x04)
            f.player.receive(Frame(0, f.player.address.logical, 0x44, b"\x6d", timestamp_ns=time.monotonic_ns()),
                             transport="virtual")
            late, = self.check(f, announcement, proof, at=standby.timestamp_ns + self.hold_ns + 10_000_000,
                               deliver=lambda: self.wire(f, f.avr, 0x44, b"\x6d"))
            row = f.avr.power_assertions[-1]
            self.assertEqual(row["kind"], "wire_no_wake")
            # Observed after the window but before the verdict, and still ignored.
            self.assertLess(row["observation_end_monotonic_ns"], late.timestamp_ns)
            self.assertLess(late.timestamp_ns, row["timestamp_ns"])
            self.assertEqual((f.tv.power, f.other.power, f.avr.power), (PowerState.ON,) * 3)

    def test_hold_counts_from_the_sources_standby_not_the_announcement(self):
        # The source's Standby follows its announcement by about 120 ms, so
        # this frame lies past a hold counted from the announcement.
        with self.assertRaisesRegex(AssertionError, "Sleeping AVR received waking command 05:44:6d"):
            with self.fixture() as f:
                announcement, standby, proof = self.thief(f)
                at = announcement.timestamp_ns + self.hold_ns + 60_000_000
                self.assertLess(at, standby.timestamp_ns + self.hold_ns)
                self.check(f, announcement, proof, at=at, deliver=lambda: self.wire(f, f.avr, 0x44, b"\x6d"))

    def test_hold_runs_to_its_end_and_drains_every_observed_wire(self):
        with self.assertRaisesRegex(AssertionError, "Sleeping TV received waking command 40:04"):
            with self.fixture() as f:
                announcement, standby, proof = self.thief(f)
                self.check(f, announcement, proof, at=standby.timestamp_ns + self.hold_ns - 1_000_000,
                           deliver=lambda: self.wire(f, f.tv, 0x04))
        waited = []
        with self.assertRaisesRegex(TimeoutError, "did not drain"):
            with self.fixture() as f:
                announcement, standby, proof = self.thief(f)
                blocked = f.avr.gpio

                class Undrained(dict):
                    def __setitem__(self, gpio, polled_ns):
                        if gpio != blocked:
                            super().__setitem__(gpio, polled_ns)

                with f.session._lock:
                    f.session._wire_idle_since_ns = Undrained(f.session._wire_idle_since_ns)
                try:
                    self.check(f, announcement, proof)
                finally:
                    waited.append(time.monotonic_ns() - standby.timestamp_ns)
        self.assertGreater(waited[0], self.hold_ns)

    def test_hold_starts_at_the_sources_observed_physical_standby(self):
        relabelled = {"kind": "state", "command": "power_off_function", "power": "on"}
        for fault in ("stale", "state", "missing", *relabelled, "other_component"):
            expected = "does not match an observed" if fault == "other_component" else "fresh physical Standby proof"
            with self.subTest(fault=fault), self.assertRaisesRegex(AssertionError, expected):
                with self.fixture() as f:
                    announcement, standby, proof = self.thief(f)
                    if fault == "stale":
                        proof = {**proof, "wire_timestamp_ns": announcement.timestamp_ns - 1}
                    elif fault == "state":
                        proof = f.player.assert_power(PowerState.STANDBY).power_assertions[-1]
                    elif fault == "missing":
                        proof = None
                    elif fault in relabelled:
                        proof = {**proof, fault: relabelled[fault]}
                    else:
                        f.tv.wait_for_power(PowerState.STANDBY, since=announcement.timestamp_ns, command="standby",
                                            broadcast=True)
                        proof = f.tv.power_assertions[-1]
                    self.check(f, announcement, proof)
        for hold in (0, -1, float("nan"), float("inf"), True, "3", None):
            with self.subTest(hold=hold), self.assertRaises(ValueError):
                with self.fixture() as f:
                    f.observation.assert_no_wake_after(None, standby=None, hold=hold)
        for wrong in ("unobserved", "failed", "virtual"):
            with self.subTest(wrong=wrong), self.assertRaisesRegex(AssertionError, "physical Active Source"):
                with self.fixture(("tv", "avr") if wrong == "unobserved" else ("tv", "avr", "player")) as f:
                    announcement, standby, proof = self.thief(f)
                    if wrong != "unobserved":
                        announcement = TxResult(announcement.frame, 0 if wrong == "failed" else 1, announcement.timestamp_ns,
                                                "virtual" if wrong == "virtual" else "wire")
                    self.check(f, announcement, proof)


class SleepingModelTests(unittest.TestCase):
    """The no-wake check rejects every frame that wakes a sleeping equipment model.

    It runs in real time, because the sweep reads the clock more often between
    waits than the virtual clock permits.
    """

    def test_every_frame_that_wakes_a_sleeping_model_is_rejected(self):
        dut = DUT("amity-dut-4")
        tv = TvComponent(dut.tentacle(0).front, "Sleeping TV")
        avr = AvrComponent(dut.tentacle(0).back, "Sleeping AVR")
        player = PlaybackComponent(avr, "Former Source", port=1)
        classify = observe_no_wake(tv, avr, player)._wakes
        waking, rejected = set(), set()
        with CecSession((tv, avr, player), backend_factory=IdleWire) as session:
            initiators = {tv.gpio: 4, avr.gpio: 0}
            addresses = sorted({component.address.physical for component in (tv, avr, player)} | {0x2000, 0xffff})
            payloads = ([b""] + [address.to_bytes(2, "big") for address in addresses]
                        + [b"\x11\x00\x04", b"\x00\x00\x01\x00"])
            # Every opcode from Amity's initiator, also with each one-byte
            # operand when directed: no model handles a one-byte broadcast.
            for component in (tv, avr, player):
                # Only the resulting power state matters, so replies are dropped.
                component.hooks = Hooks(before_tx=lambda component, frame: None)
                for destination, extra in ((component.address.logical, [bytes([value]) for value in range(256)]),
                                           (15, [])):
                    for opcode in range(256):
                        for operands in payloads + extra:
                            frame = Frame(initiators[component.gpio], destination, opcode, operands,
                                          timestamp_ns=time.monotonic_ns())
                            with session._lock:
                                component.standby()
                                component.receive(frame)
                                if component.power == PowerState.ON:
                                    waking.add((component.name, frame.raw.hex(":")))
                            if classify(component, frame):
                                rejected.add((component.name, frame.raw.hex(":")))
                with session._lock:
                    component.standby()
        self.assertEqual(waking - rejected, set())
        # Image and Text View On ask any equipment to turn on, though only the TV model obeys them.
        self.assertEqual(rejected - waking, {(avr.name, "05:04"), (avr.name, "05:0d"),
                                             (player.name, "04:04"), (player.name, "04:0d")})


if __name__ == "__main__":
    unittest.main()
