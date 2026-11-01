"""Isolation needs complete original upstream-wire observation, not final state."""

from contextlib import contextmanager
from copy import deepcopy
from dataclasses import replace
from pathlib import Path
import sys
import threading
import time
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import (AvrAttachment, AvrComponent, CecAddress, CecSession,
                           DUT, Frame, PlaybackComponent, TvComponent, TxResult)
from unit_tests.test_virtual_bus import Backend
from test_component_power_assertions import distributed_activity_fixture


class SourceIsolationAssertionTests(unittest.TestCase):
    @contextmanager
    def fixture(self):
        dut = DUT("amity-dut-4")
        tv = TvComponent(dut.tentacle(1).front, "Isolation TV")
        avr = AvrComponent(dut.tentacle(1).back, "Isolation AVR")
        player = PlaybackComponent(avr, "Isolation Play", port=1)
        upstream = AvrAttachment(avr, dut.tentacle(2).front)
        thief = PlaybackComponent(dut.tentacle(2).back, "Isolated Thief",
                                  address=CecAddress(4, "1.2.0.0"))
        components = (tv, avr, player, upstream, thief)
        with CecSession(components, backend_factory=Backend) as session:
            initiators = {component.gpio: 4 if component.direction == "front" else 0
                          for component in components}
            dut.last_scan = SimpleNamespace(initiators=initiators, _session=session)
            started = SimpleNamespace(dut=dut, session=session, started_ns=time.monotonic_ns(),
                                      initiators=initiators)
            avr.power_on()
            avr.active_input = player.address.physical
            yield dut, avr, player, upstream, thief, session, started

    def receive(self, component, physical, *, opcode=0x82, transport="wire", timestamp=None,
                source=4, destination=15):
        operands = b"" if opcode in (0x04, 0x0d) else physical.to_bytes(2, "big")
        component.receive(Frame(source, destination, opcode, operands,
                                timestamp_ns=time.monotonic_ns() if timestamp is None else timestamp),
                          transport=transport)

    def test_suppression_observes_both_wires_for_the_full_original_window(self):
        with self.fixture() as (_, avr, player, upstream, thief, _, started):
            with avr.observe_source_isolation(thief, upstream=upstream, since=started, duration=0.025) as watch:
                announcement = thief.activate()
                before = time.monotonic_ns()
                self.assertIs(watch.assert_suppressed(announcement), avr)
                self.assertGreaterEqual(time.monotonic_ns() - before, 20_000_000)
            proof = avr.routing_assertions[-1]
            self.assertEqual(proof["kind"], "wire_suppression")
            self.assertEqual(proof["observed_wires"], [avr.gpio, upstream.gpio])
            self.assertEqual(proof["observation_end_monotonic_ns"], announcement.timestamp_ns + 25_000_000)
            self.assertEqual(avr.active_input, player.address.physical)
            self.assertFalse(avr._wire_observers or upstream._wire_observers)

    def test_any_brief_physical_leak_on_either_avr_wire_fails_after_route_repair(self):
        for side in ("upstream", "output"):
            for opcode in (0x82, 0x86, 0x81, 0x04):
                with self.subTest(side=side, opcode=opcode), self.fixture() as (_, avr, player, upstream, thief, _, started):
                    with self.assertRaisesRegex(AssertionError, "reached an AVR-side"):
                        with avr.observe_source_isolation(thief, upstream=upstream, since=started, duration=0.001) as watch:
                            announcement = thief.activate()
                            receiver = upstream if side == "upstream" else avr
                            self.receive(receiver, thief.address.physical, opcode=opcode,
                                         destination=0 if opcode == 0x04 else 15)
                            avr.active_input = player.address.physical
                            watch.assert_suppressed(announcement)

    def test_original_frame_is_seen_before_hooks_drop_or_rewrite_it(self):
        for rewrite in (False, True):
            with self.subTest(rewrite=rewrite), self.fixture() as (_, avr, player, upstream, thief, _, started):
                upstream.hooks.before_rx = lambda component, frame: (
                    Frame(frame.source, 15, 0x82, player.address.physical.to_bytes(2, "big"),
                          timestamp_ns=frame.timestamp_ns) if rewrite else None)
                with self.assertRaisesRegex(AssertionError, "reached an AVR-side"):
                    with avr.observe_source_isolation(thief, upstream=upstream, since=started, duration=0.001) as watch:
                        announcement = thief.activate()
                        self.receive(upstream, thief.address.physical)
                        watch.assert_suppressed(announcement)

    def test_virtual_and_prescope_frames_do_not_count_as_current_wire_leaks(self):
        with self.fixture() as (_, avr, _, upstream, thief, _, started):
            stale = time.monotonic_ns()
            with avr.observe_source_isolation(thief, upstream=upstream, since=started, duration=0.001) as watch:
                announcement = thief.activate()
                self.receive(upstream, thief.address.physical, transport="virtual")
                self.receive(upstream, thief.address.physical, timestamp=stale)
                watch.assert_suppressed(announcement)

    def test_trigger_requires_exact_successful_physical_source_transmission(self):
        for mutation in (dict(transport="virtual"), dict(status=4), dict(timestamp_ns=0),
                         dict(frame=Frame(4, 15, 0x84, b"\x12\x00\x04"))):
            with self.subTest(mutation=mutation), self.fixture() as (_, avr, _, upstream, thief, _, started):
                with self.assertRaises(AssertionError):
                    with avr.observe_source_isolation(thief, upstream=upstream, since=started, duration=0.001) as watch:
                        announcement = thief.activate()
                        watch.assert_suppressed(replace(announcement, **mutation))

    def test_slow_inspection_does_not_restart_the_observation_window(self):
        with self.fixture() as (_, avr, _, upstream, thief, _, started):
            with avr.observe_source_isolation(thief, upstream=upstream, since=started, duration=0.01) as watch:
                announcement = thief.activate()
                time.sleep(0.015)
                watch.assert_suppressed(announcement)
            self.assertEqual(avr.routing_assertions[-1]["observation_end_monotonic_ns"],
                             announcement.timestamp_ns + 10_000_000)

    def test_session_loss_or_missing_wire_clock_cannot_prove_absence(self):
        for lost in (True, False):
            with self.subTest(lost=lost), self.fixture() as (_, avr, _, upstream, thief, session, started):
                with self.assertRaises(AssertionError):
                    with avr.observe_source_isolation(thief, upstream=upstream, since=started, duration=0.001) as watch:
                        announcement = thief.activate()
                        if lost:
                            session._stop.set()
                        else:
                            self.receive(upstream, thief.address.physical, timestamp=0)
                        watch.assert_suppressed(announcement)

    def test_context_cannot_succeed_without_the_suppression_assertion(self):
        with self.fixture() as (_, avr, _, upstream, thief, _, started):
            with self.assertRaisesRegex(RuntimeError, "not asserted"):
                with avr.observe_source_isolation(thief, upstream=upstream, since=started):
                    pass
            self.assertFalse(avr._wire_observers or upstream._wire_observers)

    def test_delayed_delivery_of_a_window_frame_is_seen_before_idle_coverage(self):
        with self.fixture() as (_, avr, _, upstream, thief, _, started):
            with self.assertRaisesRegex(AssertionError, "reached an AVR-side"):
                with avr.observe_source_isolation(thief, upstream=upstream, since=started, duration=0.001) as watch:
                    announcement = thief.activate()
                    # The physical frame completes in the observation window,
                    # but a queued/SSH-delayed delivery reaches Python later.
                    timer = threading.Timer(0.01, lambda: self.receive(
                        upstream, thief.address.physical, timestamp=announcement.timestamp_ns + 1000))
                    timer.start()
                    try:
                        watch.assert_suppressed(announcement)
                    finally:
                        timer.join()

    def test_missing_postwindow_idle_receive_cannot_prove_absence(self):
        with self.fixture() as (_, avr, _, upstream, thief, _, started):
            avr._power_timeout = 0.005
            with self.assertRaisesRegex(TimeoutError, "coverage did not drain"):
                with avr.observe_source_isolation(thief, upstream=upstream, since=started, duration=0.001) as watch:
                    watch.assert_suppressed(thief.activate())

    def test_active_source_positive_control_needs_fresh_applied_dut_wire_frame(self):
        with self.fixture() as (_, avr, _, upstream, thief, _, started):
            upstream.active_input = thief.address.physical
            self.receive(upstream, thief.address.physical, transport="virtual")
            self.receive(upstream, thief.address.physical, timestamp=started.started_ns - 1)
            self.receive(upstream, thief.address.physical, source=8)
            with self.assertRaises(TimeoutError):
                upstream.wait_for_active_source(thief, since=started, timeout=0.001)
            self.receive(upstream, thief.address.physical)
            self.assertIs(upstream.wait_for_active_source(thief, since=started, timeout=0.05), upstream)
            self.assertEqual(avr.active_input, thief.address.physical)
            self.assertEqual(upstream.routing_assertions[-1]["command"], "active_source")

    def test_owner_boot_is_rechecked_after_observation_outside_responder_locks(self):
        f = distributed_activity_fixture()
        action = f.activity(display=f.member_tv, source=f.member_player, audio=f.member_avr).start()
        calls = []

        def system():
            self.assertFalse(f.session._lock._is_owned())
            calls.append(None)
            result = deepcopy(f.source.system)
            if len(calls) > 4:
                result["membership"]["peers"][0]["boot_id"] = "e" * 32
            return result

        f.dut.get_system = system
        with self.assertRaises(AssertionError):
            with f.avr.observe_source_isolation(f.member_player, upstream=f.member_tv,
                                                since=action, duration=0.001) as watch:
                timestamp = time.monotonic_ns()
                frame = Frame(f.member_player.address.logical, 15, 0x82,
                              f.member_player.address.physical.to_bytes(2, "big"))
                announcement = TxResult(frame, 1, timestamp)
                f.session.records = (*f.session.records,
                    {"component": f.member_player.name, "gpio": f.member_player.gpio,
                     "direction": "tx", "transport": "wire", "ok": True,
                     "frame": frame.raw.hex(":"), "wire_timestamp_ns": timestamp})
                f.session._wire_idle_since_ns = {}
                timer = threading.Timer(0.01, lambda: f.session._wire_idle_since_ns.update(
                    {receiver.gpio: time.monotonic_ns() for receiver in (f.avr, f.member_tv)}))
                timer.start()
                try:
                    watch.assert_suppressed(announcement)
                finally:
                    timer.join()
        self.assertGreaterEqual(len(calls), 5)


if __name__ == "__main__":
    unittest.main()
