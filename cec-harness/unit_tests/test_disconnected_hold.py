"""Lost HTTP callers cannot qualify stale or unfinished physical key streams."""

from dataclasses import replace
import http.client
import json
from pathlib import Path
import socket
import ssl
import sys
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from urllib.request import Request

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import DUT, Frame, Key
from amity_harness.http_transport import HTTPSOpener
from amity_harness.testbed import TestBed
import test_keys as key_fixture
import test_testbed as bed_fixture


class SocketPairFactory:
    """Exercise HTTP framing and real shutdown without a network listener."""

    def __init__(self):
        self.connections, self.peers = [], []

    def __call__(self, host, port, *, timeout, context):
        connection = http.client.HTTPConnection(host, port, timeout=timeout)

        def connect():
            connection.sock, peer = socket.socketpair()
            connection.sock.settimeout(timeout)
            peer.settimeout(timeout)
            self.peers.append(peer)

        connection.connect = connect
        self.connections.append(connection)
        return connection

    def close(self):
        for peer in self.peers:
            peer.close()


class DisconnectTransportTests(unittest.TestCase):
    def test_complete_authenticated_request_precedes_observation_and_real_socket_eof(self):
        factory = SocketPairFactory()
        self.addCleanup(factory.close)
        opener = HTTPSOpener("https://amity.local", ssl.create_default_context(), connection_factory=factory)
        request = Request("https://amity.local/api/key", data=b'{"key":65,"duration_ms":3000}',
                          headers={"Authorization": "Bearer PRIVATE", "Content-Type": "application/json"})

        def observe():
            wire = b""
            while b'{"key":65,"duration_ms":3000}' not in wire:
                wire += factory.peers[0].recv(4096)
            self.assertIn(b"POST /api/key HTTP/1.1", wire)
            self.assertIn(b"Authorization: Bearer PRIVATE", wire)
            self.assertIsNotNone(factory.connections[0].sock)
            return "physical press"

        self.assertEqual(opener.disconnect_after(request, observe, timeout=1), "physical press")
        self.assertEqual(factory.peers[0].recv(1), b"")
        self.assertIsNone(opener._connection)
        self.assertFalse(opener._lock.locked())
        self.assertEqual(len(factory.connections), 1)  # No mutation replay.

    def test_observer_failure_still_disconnects_and_releases_transport(self):
        factory = SocketPairFactory()
        self.addCleanup(factory.close)
        opener = HTTPSOpener("https://amity.local", ssl.create_default_context(), connection_factory=factory)

        def fail():
            factory.peers[0].recv(4096)
            raise TimeoutError("No physical admission")

        with self.assertRaisesRegex(TimeoutError, "physical admission"):
            opener.disconnect_after(Request("https://amity.local/api/key", data=b"{}"), fail, timeout=1)
        self.assertEqual(factory.peers[0].recv(1), b"")
        self.assertFalse(opener._lock.locked())
        self.assertEqual(len(factory.connections), 1)

    def test_foreign_origin_and_unauthenticated_dut_cannot_send(self):
        factory = SocketPairFactory()
        self.addCleanup(factory.close)
        opener = HTTPSOpener("https://amity.local", ssl.create_default_context(), connection_factory=factory)
        with self.assertRaises(ValueError):
            opener.disconnect_after(Request("https://other.local/api/key", data=b"{}"), lambda: None)
        self.assertFalse(factory.connections)
        dut = DUT("amity-dut", opener=opener)
        with self.assertRaises(RuntimeError):
            dut._disconnect_key_request({"key": 65, "duration_ms": 3000}, lambda: None, timeout=1)
        self.assertFalse(factory.connections)

    def test_dut_uses_current_bearer_and_only_the_shipping_key_route(self):
        captured = []

        class Opener:
            def disconnect_after(self, request, observer, *, timeout):
                captured.append((request, timeout))
                return observer()

        dut = DUT("amity-dut", opener=Opener())
        dut._token = "PRIVATE_SESSION"
        result = dut._disconnect_key_request({"key": 65, "duration_ms": 3000}, lambda: "admitted", timeout=3)
        request, timeout = captured[0]
        self.assertEqual(result, "admitted")
        self.assertEqual(request.full_url, dut.url + "/api/key")
        self.assertEqual(request.get_header("Authorization"), "Bearer PRIVATE_SESSION")
        self.assertEqual(json.loads(request.data), {"key": 65, "duration_ms": 3000})
        self.assertEqual(timeout, 3)


class DisconnectedHoldTests(unittest.TestCase):
    def setUp(self):
        # Advance the simulated duration without sleeping for every positive
        # hold. All frame and key-helper clocks share the same monotonic offset.
        real_monotonic_ns = time.monotonic_ns
        self.clock_offset_ns = 0
        clock = patch("amity_harness.keys.time.monotonic_ns",
                      lambda: real_monotonic_ns() + self.clock_offset_ns)
        clock.start()
        self.addCleanup(clock.stop)

    def fixture(self, *, remote=False):
        helper = key_fixture.KeyTests()
        f = helper.fixture(remote=remote)
        f.dut._runtime = replace(f.dut._runtime, cec_timeout=.05)
        f.target = f.member_avr if remote else f.avr
        f.session._backends = {component.gpio: SimpleNamespace()
                               for component in f.source.components}
        f.session._wire_idle_since_ns = {}
        f.threads = []
        f.receive_key = lambda **kwargs: helper.receive(f, f.target, Key.VOLUME_UP, **kwargs)
        self.addCleanup(lambda: [thread.join(1) for thread in f.threads])
        return f

    def request(self, f, *, first="wire", tail="release"):
        def send(data, observe, *, timeout):
            f.calls.append(("/api/key", data))
            # Admission itself may not use HTTP: the real handler is occupied.
            old_get_state = f.dut.get_state
            f.dut.get_state = lambda: self.fail("HTTP during pending hold")
            try:
                f.receive_key(transport=first, release=False)
                observe()
            finally:
                f.dut.get_state = old_get_state

            def finish():
                time.sleep(.003)  # Must be after the simulated client close.
                if tail != "early_release":
                    self.clock_offset_ns += data["duration_ms"] * 1_000_000
                    if tail == "uncertain_release":
                        self.clock_offset_ns -= 20_000_000
                f.receive_key(release=tail != "missing")
                if tail in ("late_press", "late_released"):
                    time.sleep(.015)
                    f.receive_key(release=tail == "late_released")
                if tail == "owner_changed":
                    f.source.system["membership"]["peers"][0]["boot_id"] = "e" * 32
                # Stand in for a completed backend receive poll after the
                # observation boundary, not merely an empty record list.
                time.sleep(.06)
                f.session._wire_idle_since_ns = {c.gpio: time.monotonic_ns() for c in f.source.components}

            thread = threading.Thread(target=finish)
            f.threads.append(thread)
            thread.start()
        f.dut._disconnect_key_request = send

    def test_local_and_member_holds_release_after_disconnect_then_accept_fresh_key(self):
        for remote in (False, True):
            with self.subTest(remote=remote):
                f = self.fixture(remote=remote)
                self.request(f)
                action = f.dut.press_key_disconnected(Key.VOLUME_UP, f.target, duration_ms=1000)
                self.assertEqual(f.calls[0], ("/api/key", {"key": 65, "duration_ms": 1000}))
                self.assertLess(action.trace["first_press_wire_timestamp_ns"], action.trace["disconnected_monotonic_ns"])
                self.assertLess(action.trace["disconnected_monotonic_ns"], action.trace["release_wire_timestamp_ns"])
                self.assertGreaterEqual(action.trace["release_wire_timestamp_ns"],
                                        action.trace["hold_deadline_monotonic_ns"])
                self.assertTrue(action.trace["cleanup_verified"])
                self.assertEqual(action.owner_boots, f.dut.last_scan.owner_boots)
                next_action = f.dut.press_key(Key.VOLUME_UP)
                f.receive_key()
                next_action.assert_received_by(f.target, timeout=.05)

    def test_missing_release_and_press_after_release_cannot_pass(self):
        for tail in ("missing", "late_press", "late_released"):
            with self.subTest(tail=tail):
                f = self.fixture()
                self.request(f, tail=tail)
                if tail == "late_released":
                    original = f.dut.get_state

                    def slow_owner_proof():
                        # Let both pairs reach history before the generic
                        # press/release assertion inspects its final pair.
                        if f.calls:
                            time.sleep(.025)
                        return original()

                    f.dut.get_state = slow_owner_proof
                if tail == "missing":
                    f.dut._runtime = replace(f.dut._runtime, cec_timeout=.001)
                with self.assertRaises((AssertionError, TimeoutError)):
                    f.dut.press_key_disconnected(Key.VOLUME_UP, f.target, duration_ms=1000)

    def test_post_disconnect_repeat_does_not_hide_premature_release(self):
        for remote in (False, True):
            with self.subTest(remote=remote):
                f = self.fixture(remote=remote)
                self.request(f, tail="early_release")
                with self.assertRaisesRegex(AssertionError, "released before its requested duration"):
                    f.dut.press_key_disconnected(Key.VOLUME_UP, f.target, duration_ms=1000)

    def test_missing_physical_backend_fails_before_sending(self):
        f = self.fixture()
        f.session._backends.clear()
        self.request(f)
        with self.assertRaisesRegex(AssertionError, "no physical wire clock"):
            f.dut.press_key_disconnected(Key.VOLUME_UP, f.target, duration_ms=1000)
        self.assertFalse(f.calls)

    def test_report_retains_disconnect_repetition_and_drain_proof_without_private_data(self):
        f = self.fixture(remote=True)
        self.request(f)
        action = f.dut.press_key_disconnected(Key.VOLUME_UP, f.target, duration_ms=1000)
        trace = action.trace
        log = []
        dut = bed_fixture.UnitDUT("first-board", log)
        dut.key_operations = [{"component": "prior bed", "token": "PRIVATE_OLD_TOKEN"}]
        bed = TestBed(dut, session_factory=lambda components: bed_fixture.UnitSession(components, log))
        with bed:
            dut.key_operations.append({**trace, "token": "PRIVATE_TOKEN", "password": "PRIVATE_PASSWORD",
                                       "headers": {"Authorization": "PRIVATE_BEARER"}})
        self.assertEqual(len(bed.report["keys"]), 1)
        report = bed.report["keys"][0]
        for field in ("disconnected", "first_press_wire_timestamp_ns", "disconnected_monotonic_ns",
                      "repeat_after_disconnect_wire_timestamp_ns", "observation_end_monotonic_ns",
                      "observed_wires", "idle_receive_since_ns", "cleanup_verified", "presses",
                      "press_wire_timestamp_ns", "release_wire_timestamp_ns", "deadline_monotonic_ns",
                      "hold_deadline_monotonic_ns"):
            self.assertEqual(report[field], trace[field], field)
        self.assertLess(report["first_press_wire_timestamp_ns"], report["disconnected_monotonic_ns"])
        self.assertLess(report["disconnected_monotonic_ns"], report["repeat_after_disconnect_wire_timestamp_ns"])
        self.assertLess(report["repeat_after_disconnect_wire_timestamp_ns"], report["release_wire_timestamp_ns"])
        self.assertGreaterEqual(report["release_wire_timestamp_ns"], report["hold_deadline_monotonic_ns"])
        self.assertTrue(all(stamp >= report["observation_end_monotonic_ns"]
                            for stamp in report["idle_receive_since_ns"].values()))
        self.assertNotIn("PRIVATE_", repr(bed.report))
        report["idle_receive_since_ns"][str(report["observed_wires"][0])] = 0
        self.assertTrue(all(stamp > 0 for stamp in bed.report["keys"][0]["idle_receive_since_ns"].values()))

    def test_old_member_boot_invalidates_disconnected_cleanup(self):
        f = self.fixture(remote=True)
        self.request(f, tail="owner_changed")
        with self.assertRaises(AssertionError):
            f.dut.press_key_disconnected(Key.VOLUME_UP, f.target, duration_ms=1000)

    def test_virtual_press_is_not_admission_and_invalid_duration_never_sends(self):
        f = self.fixture()
        for value in (0, 999, 5001, True):
            with self.subTest(value=value), self.assertRaises(ValueError):
                f.dut.press_key_disconnected(Key.VOLUME_UP, f.target, duration_ms=value)
        self.assertFalse(f.calls)
        for key in (Key.POWER, Key.PLAY_PAUSE):
            with self.subTest(key=key), self.assertRaises(ValueError):
                f.dut.press_key_disconnected(key, f.target)
        self.request(f, first="virtual")
        with self.assertRaises(TimeoutError):
            f.dut.press_key_disconnected(Key.VOLUME_UP, f.target, duration_ms=1000)
        self.assertEqual(len(f.calls), 1)


if __name__ == "__main__":
    unittest.main()
