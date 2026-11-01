"""Source-theft bounds use complete physical frames, independently of UI time."""

from copy import deepcopy
from dataclasses import replace
from pathlib import Path
import sys
import threading
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import Frame, TxResult
from test_component_power_assertions import distributed_activity_fixture


class SourceTheftDeadlineTests(unittest.TestCase):
    def fixture(self):
        fixture = distributed_activity_fixture()
        fixture.activity(display=fixture.member_tv, source=fixture.member_player, audio=fixture.member_avr).start()
        player = fixture.member_player
        player.power_on()
        player.active = True
        timestamp = time.monotonic_ns()
        frame = Frame(player.address.logical, 15, 0x82, player.address.physical.to_bytes(2, "big"))
        announcement = TxResult(frame, 1, timestamp)
        fixture.session.records = (*fixture.session.records,
            {"component": player.name, "gpio": player.gpio, "direction": "tx", "transport": "wire",
             "frame": frame.raw.hex(":"), "wire_timestamp_ns": timestamp, "ok": True})
        return fixture, announcement

    def standby(self, fixture, announcement, elapsed_ns, *, transport="wire"):
        player = fixture.member_player
        player.receive(Frame(0, player.address.logical, 0x36,
                             timestamp_ns=announcement.timestamp_ns + elapsed_ns), transport=transport)

    def test_late_standby_already_in_history_cannot_satisfy_the_250_ms_bound(self):
        fixture, announcement = self.fixture()
        self.standby(fixture, announcement, 250_000_001)
        clock = announcement.timestamp_ns / 1_000_000_000 + 3
        with patch("amity_harness.components.time.monotonic", return_value=clock), self.assertRaises(TimeoutError):
            fixture.member_player.wait_for_standby_after(announcement)
        self.assertEqual(fixture.member_player.power.value, "standby")
        self.assertEqual(fixture.member_player.power_assertions, ())

    def test_standby_accepts_complete_frames_up_to_the_250_ms_boundary(self):
        for elapsed_ns in (249_999_999, 250_000_000):
            with self.subTest(elapsed_ns=elapsed_ns):
                fixture, announcement = self.fixture()
                self.standby(fixture, announcement, elapsed_ns)
                clock = announcement.timestamp_ns / 1_000_000_000 + 3
                with patch("amity_harness.components.time.monotonic", return_value=clock):
                    fixture.member_player.wait_for_standby_after(announcement)
                self.assertEqual(fixture.member_player.power_assertions[-1]["response_latency_ns"], elapsed_ns)

    def test_timely_standby_remains_valid_after_slow_real_owner_proof(self):
        fixture, announcement = self.fixture()
        self.standby(fixture, announcement, 125_000_000)
        clock = [announcement.timestamp_ns / 1_000_000_000]

        def slow_system():
            clock[0] += 3
            return deepcopy(fixture.source.system)

        fixture.dut.get_system = slow_system
        with patch("amity_harness.components.time.monotonic", side_effect=lambda: clock[0]):
            self.assertIs(fixture.member_player.wait_for_standby_after(announcement), fixture.member_player)
        self.assertGreater(clock[0] - announcement.timestamp_ns / 1_000_000_000, 2)
        assertion = fixture.member_player.power_assertions[-1]
        self.assertEqual(assertion["trigger_wire_timestamp_ns"], announcement.timestamp_ns)
        self.assertEqual(assertion["response_latency_ns"], 125_000_000)
        self.assertEqual(assertion["max_elapsed_ns"], 250_000_000)
        self.assertEqual(assertion["destination"], fixture.member_player.address.logical)

    def test_virtual_or_local_standby_does_not_prove_remediation(self):
        for transport in ("virtual", "local"):
            with self.subTest(transport=transport):
                fixture, announcement = self.fixture()
                if transport == "local":
                    fixture.member_player.standby()
                else:
                    self.standby(fixture, announcement, 100_000_000, transport=transport)
                clock = announcement.timestamp_ns / 1_000_000_000 + 3
                with patch("amity_harness.components.time.monotonic", return_value=clock), self.assertRaises(TimeoutError):
                    fixture.member_player.wait_for_standby_after(announcement)

    def test_trigger_requires_exact_component_session_wire_and_successful_active_source(self):
        fixture, announcement = self.fixture()
        invalid = (replace(announcement, transport="virtual"), replace(announcement, status=4),
                   replace(announcement, timestamp_ns=0), replace(announcement, timestamp_ns=announcement.timestamp_ns + 1),
                   replace(announcement, frame=Frame(4, 15, 0x84, b"\x11\x00\x04")))
        for trigger in invalid:
            with self.subTest(trigger=trigger), self.assertRaises(AssertionError):
                fixture.member_player.wait_for_standby_after(trigger)
        for component in (fixture.member_avr, fixture.member_tv, fixture.player):
            with self.subTest(component=component.name), self.assertRaises(AssertionError):
                component.wait_for_standby_after(announcement)
        other, _ = self.fixture()
        with self.assertRaises(AssertionError):
            other.member_player.wait_for_standby_after(announcement)

    def test_missing_response_ends_at_absolute_wire_deadline(self):
        fixture, announcement = self.fixture()
        clock = [announcement.timestamp_ns / 1_000_000_000]
        beginning = clock[0]

        def wait(seconds):
            clock[0] += seconds

        with patch("amity_harness.components.time.monotonic", side_effect=lambda: clock[0]), \
                patch.object(fixture.member_player._power_condition, "wait", side_effect=wait), self.assertRaises(TimeoutError):
            fixture.member_player.wait_for_standby_after(announcement)
        self.assertGreaterEqual(clock[0] - beginning, 0.249)
        self.assertLessEqual(clock[0] - beginning, 0.251)

    def test_route_restoration_uses_the_same_original_250_ms_window(self):
        for elapsed_ns, expected_success in ((125_000_000, True), (249_999_999, True),
                                             (250_000_000, True), (250_000_001, False),
                                             (500_000_000, False)):
            with self.subTest(elapsed_ns=elapsed_ns):
                fixture, announcement = self.fixture()
                avr, player = fixture.member_avr, fixture.member_player
                avr.receive(Frame(0, 15, 0x86, player.address.physical.to_bytes(2, "big"),
                                  timestamp_ns=announcement.timestamp_ns + elapsed_ns))
                clock = [announcement.timestamp_ns / 1_000_000_000 + 3]

                def slow_system():
                    clock[0] += 3
                    return deepcopy(fixture.source.system)

                fixture.dut.get_system = slow_system
                with patch("amity_harness.components.time.monotonic", side_effect=lambda: clock[0]):
                    if expected_success:
                        avr.wait_for_input(player, since=announcement.timestamp_ns, after=announcement, within=0.250)
                        self.assertEqual(avr.routing_assertions[-1]["response_latency_ns"], elapsed_ns)
                        self.assertEqual(avr.routing_assertions[-1]["max_elapsed_ns"], 250_000_000)
                    else:
                        with self.assertRaises(TimeoutError):
                            avr.wait_for_input(player, since=announcement.timestamp_ns, after=announcement, within=0.250)

    def locked_route(self, *, elapsed=0.125, transport="wire", timeout=0.8, expected_success=True):
        """Dispatch a route, retaining the real session lock past the wire window."""
        fixture, announcement = self.fixture()
        avr, player = fixture.member_avr, fixture.member_player
        held, release = threading.Event(), threading.Event()
        errors, timestamps = [], []

        def dispatch():
            try:
                with fixture.session._lock:
                    held.set()
                    remaining = announcement.timestamp_ns / 1_000_000_000 + elapsed - time.monotonic()
                    if release.wait(max(0, remaining)):
                        return
                    timestamp = time.monotonic_ns()
                    timestamps.append(timestamp)
                    avr.receive(Frame(0, 15, 0x86, player.address.physical.to_bytes(2, "big"),
                                      timestamp_ns=timestamp), transport=transport)
                    # A synchronous playback response can retain the dispatch
                    # lock after the DUT's route was physically complete.
                    remaining = announcement.timestamp_ns / 1_000_000_000 + 0.35 - time.monotonic()
                    release.wait(max(0, remaining))
            except BaseException as error:
                errors.append(error)

        worker = threading.Thread(target=dispatch)
        worker.start()
        self.assertTrue(held.wait(0.5))
        try:
            if expected_success:
                self.assertIs(avr.wait_for_input(player, since=announcement.timestamp_ns, after=announcement,
                                                within=0.250, timeout=timeout), avr)
                self.assertGreaterEqual(time.monotonic_ns() - announcement.timestamp_ns, 300_000_000)
                assertion = avr.routing_assertions[-1]
                self.assertEqual(assertion["trigger_wire_timestamp_ns"], announcement.timestamp_ns)
                self.assertEqual(assertion["wire_timestamp_ns"], timestamps[0])
                self.assertEqual(assertion["response_latency_ns"], timestamps[0] - announcement.timestamp_ns)
                self.assertLessEqual(assertion["response_latency_ns"], 250_000_000)
                self.assertEqual(assertion["max_elapsed_ns"], 250_000_000)
            else:
                with self.assertRaises(TimeoutError):
                    avr.wait_for_input(player, since=announcement.timestamp_ns, after=announcement,
                                       within=0.250, timeout=timeout)
                self.assertEqual(avr.routing_assertions, ())
        finally:
            release.set()
            worker.join(1)
        self.assertFalse(worker.is_alive())
        self.assertEqual(errors, [])

    def test_timely_route_is_inspected_after_dispatch_lock_outlasts_wire_window(self):
        self.locked_route()

    def test_251_ms_route_is_rejected_when_inspected_after_dispatch_lock(self):
        self.locked_route(elapsed=0.251, expected_success=False)

    def test_virtual_route_cannot_pass_when_dispatch_lock_outlasts_wire_window(self):
        self.locked_route(transport="virtual", expected_success=False)

    def test_timed_route_lock_inspection_still_has_the_ordinary_timeout_budget(self):
        started = time.monotonic()
        self.locked_route(timeout=0.04, expected_success=False)
        self.assertLess(time.monotonic() - started, 0.2)

    def test_missing_route_ends_at_absolute_wire_deadline_with_uncontended_lock(self):
        fixture, announcement = self.fixture()
        clock = [announcement.timestamp_ns / 1_000_000_000]
        beginning = clock[0]

        def wait(seconds):
            clock[0] += seconds

        with patch("amity_harness.components.time.monotonic", side_effect=lambda: clock[0]), \
                patch.object(fixture.member_avr._power_condition, "wait", side_effect=wait), self.assertRaises(TimeoutError):
            fixture.member_avr.wait_for_input(fixture.member_player, since=announcement.timestamp_ns,
                                              after=announcement, within=0.250)
        self.assertGreaterEqual(clock[0] - beginning, 0.249)
        self.assertLessEqual(clock[0] - beginning, 0.251)
        self.assertEqual(fixture.member_avr.routing_assertions, ())

    def test_timely_route_can_be_inspected_after_owner_proof_exceeds_wait_budget(self):
        for elapsed_ns, expected_success in ((125_000_000, True), (251_000_000, False)):
            with self.subTest(elapsed_ns=elapsed_ns):
                fixture, announcement = self.fixture()
                avr, player = fixture.member_avr, fixture.member_player
                avr.receive(Frame(0, 15, 0x86, player.address.physical.to_bytes(2, "big"),
                                  timestamp_ns=announcement.timestamp_ns + elapsed_ns))
                clock = [announcement.timestamp_ns / 1_000_000_000]
                proofs = []

                def slow_system():
                    proofs.append(clock[0])
                    clock[0] += 6
                    return deepcopy(fixture.source.system)

                fixture.dut.get_system = slow_system
                with patch("amity_harness.components.time.monotonic", side_effect=lambda: clock[0]):
                    if expected_success:
                        avr.wait_for_input(player, since=announcement.timestamp_ns, after=announcement,
                                           within=0.250, timeout=5)
                        self.assertEqual(avr.routing_assertions[-1]["response_latency_ns"], elapsed_ns)
                        self.assertEqual(avr.routing_assertions[-1]["max_elapsed_ns"], 250_000_000)
                        self.assertGreaterEqual(len(proofs), 2)
                        self.assertGreaterEqual(clock[0] - proofs[1], 6)
                    else:
                        with self.assertRaises(TimeoutError):
                            avr.wait_for_input(player, since=announcement.timestamp_ns, after=announcement,
                                               within=0.250, timeout=5)
                        self.assertEqual(avr.routing_assertions, ())

    def test_source_deadline_cannot_be_extended_above_250_ms(self):
        fixture, announcement = self.fixture()
        for within in (0, -1, True, 0.250000001, 0.251, 1, 2, float("inf"), float("nan")):
            with self.subTest(within=within):
                with self.assertRaises(ValueError):
                    fixture.member_player.wait_for_standby_after(announcement, within=within)
                with self.assertRaises(ValueError):
                    fixture.member_avr.wait_for_input(fixture.member_player, since=announcement.timestamp_ns,
                                                      after=announcement, within=within)


if __name__ == "__main__":
    unittest.main()
