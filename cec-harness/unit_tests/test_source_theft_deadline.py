"""Source-theft bounds use complete physical frames, independently of UI time."""

from copy import deepcopy
from dataclasses import replace
from pathlib import Path
import sys
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

    def test_late_standby_already_in_history_cannot_satisfy_the_one_second_bound(self):
        fixture, announcement = self.fixture()
        self.standby(fixture, announcement, 1_000_000_001)
        clock = announcement.timestamp_ns / 1_000_000_000 + 3
        with patch("amity_harness.components.time.monotonic", return_value=clock), self.assertRaises(TimeoutError):
            fixture.member_player.wait_for_standby_after(announcement)
        self.assertEqual(fixture.member_player.power.value, "standby")
        self.assertEqual(fixture.member_player.power_assertions, ())

    def test_standby_accepts_complete_frames_up_to_the_one_second_boundary(self):
        for elapsed_ns in (999_999_999, 1_000_000_000):
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
        self.assertEqual(assertion["max_elapsed_ns"], 1_000_000_000)
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
        self.assertGreaterEqual(clock[0] - beginning, 0.99)
        self.assertLessEqual(clock[0] - beginning, 1.01)

    def test_route_restoration_uses_the_same_original_one_second_window(self):
        for elapsed_ns, expected_success in ((250_000_000, True), (1_000_000_000, True),
                                             (1_000_000_001, False), (1_500_000_000, False)):
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
                        avr.wait_for_input(player, since=announcement.timestamp_ns, after=announcement, within=1)
                        self.assertEqual(avr.routing_assertions[-1]["response_latency_ns"], elapsed_ns)
                    else:
                        with self.assertRaises(TimeoutError):
                            avr.wait_for_input(player, since=announcement.timestamp_ns, after=announcement, within=1)

    def test_source_deadline_cannot_be_extended_above_one_second(self):
        fixture, announcement = self.fixture()
        for within in (0, -1, True, 1.001, 2, float("inf"), float("nan")):
            with self.subTest(within=within):
                with self.assertRaises(ValueError):
                    fixture.member_player.wait_for_standby_after(announcement, within=within)
                with self.assertRaises(ValueError):
                    fixture.member_avr.wait_for_input(fixture.member_player, since=announcement.timestamp_ns,
                                                      after=announcement, within=within)


if __name__ == "__main__":
    unittest.main()
