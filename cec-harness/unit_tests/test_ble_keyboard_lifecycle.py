"""Repeated UI scans preserve one advertiser and require real CEC key evidence."""

from copy import deepcopy
from contextlib import contextmanager, redirect_stdout
from dataclasses import replace
import io
from pathlib import Path
import runpy
import sys
import time
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import CecSession, Frame, PowerState
from amity_harness.testbed import TestBedCleanupError
from amity_harness.config import track_testbeds, use_config
from amity_harness.dut import AmityHTTPError
from functional_tests import ble_keyboard
from unit_tests.test_ble_input import KeyWire
from unit_tests.test_ble_keyboard import Fixture


class LifecycleFixture(Fixture):
    def __init__(self):
        super().__init__()
        self.events, self.source, self.scan, self.active = [], None, None, False
        self.main._testbed = None
        self.main.last_scan = None
        self.main.authenticate = Mock()
        self.main.press_fw_reset = Mock(side_effect=AssertionError("No pairing reboot workaround"))
        self.main.capture_logs = Mock(side_effect=AssertionError("Pairing uses only UI state"))
        self.main._runtime = replace(self.main._runtime, cec_timeout=.04,
                                    session_factory=lambda components: CecSession(components, backend_factory=KeyWire))
        self.main.scan_hdmi = self.scan_hdmi
        self.main.create_activity = self.create_activity
        self.main.get_configuration = lambda: {"activities": [
            {"activity_id": "d" * 32}, {"activity_id": "e" * 32}, {"activity_id": "f" * 32}]}
        self.main.wait_for = self.wait_for
        self.slot, self.selector, self.readiness = 2, None, []
        self.activity["current_activity"] = -1
        original_report = self.worker.report
        self.held = False
        self.missing_key = False

        def report(*, keyboard=(), consumer=0):
            result = original_report(keyboard=keyboard, consumer=consumer)
            selectors = [usage for usage in keyboard if 0x3A <= usage <= 0x3D]
            if selectors:
                assert selectors == [0x3A + self.slot], "Use the actual saved activity slot"
                self.selector = selectors[0]
            elif self.selector is not None:
                self.selector = None
                assert not self.active and self.source is not None
                self.active = True
                self.activity["activity_settling"] = True
                self.emit(0x44, b"\x6d")
                self.emit(0x45)
                self.events.append(("activity.start", self.remote_scans))
            select = 0x28 in keyboard
            if select != self.held:
                if not self.active or self.source is None:
                    raise AssertionError("BLE input must use the saved active source")
                self.events.append(("tap.down" if select else "tap.up", self.remote_scans))
                if not self.missing_key:
                    self.emit(0x44 if select else 0x45, b"\x00" if select else b"")
                self.held = select
            return result

        self.worker.report = report

    def wait_for(self, predicate, *, timeout):
        # Wire power precedes the local activity commit. The test must wait
        # for shipping current/settling state rather than treat power as ready.
        self.readiness.append(predicate(deepcopy(self.activity)))
        self.activity.update(current_activity=self.slot, activity_settling=False)
        self.readiness.append(predicate(deepcopy(self.activity)))
        assert self.readiness[-2:] == [False, True]
        return deepcopy(self.activity)

    @property
    def remote_scans(self):
        return sum(path == "/api/remotes/scan" for path, _ in self.calls)

    def emit(self, opcode, operands=b""):
        self.source.receive(Frame(1, self.source.address.logical, opcode, operands,
                                  time.monotonic_ns()), transport="wire")

    def scan_hdmi(self):
        assert self.remote_scans == 4 and self.state["bonded"] and self.state["connected"]
        bed = self.main._testbed
        self.source, = bed.components
        self.source.assert_power(PowerState.STANDBY)
        self.events.append(("hdmi.scan", self.remote_scans))
        scan = SimpleNamespace(dut=self.main, _boot=self.boot, _session=bed.session, _testbed=bed,
                               expected=(self.source,), initiators={self.source.gpio: 1},
                               owner_boots={self.main.identity.device_id: self.boot},
                               owner_duts={self.main.identity.device_id: self.main},
                               validate_owners=lambda components: None)

        def contains(*components):
            assert components == (self.source,)
            return scan

        def on(component, endpoint):
            assert component is self.source and endpoint == self.source.endpoint
            return scan

        scan.assert_contains_only, scan.assert_on = contains, on
        self.scan = self.main.last_scan = scan
        return scan

    @contextmanager
    def create_activity(self, name, *, display, source):
        assert self.remote_scans == 4 and source is self.source and display is None
        self.events.append(("activity.create", self.remote_scans))
        fixture = self

        class Activity:
            activity_id = "f" * 32

            def start(self):
                raise AssertionError("Lifecycle setup must start through the real BLE selector")

            def assert_active(self):
                assert fixture.active and fixture.activity["current_activity"] == fixture.slot
                assert fixture.activity["activity_settling"] is False

            def standby(self):
                assert fixture.active
                marker = time.monotonic_ns()
                fixture.emit(0x36)
                fixture.active = False
                fixture.activity["current_activity"] = -1
                fixture.events.append(("activity.standby", fixture.remote_scans))
                return marker

        activity = Activity()
        try:
            yield activity
        finally:
            if self.active:
                assert activity._needs_standby, "A BLE-started activity must retain owned cleanup"
                activity.standby()
            self.events.append(("activity.remove", self.remote_scans))


class BLEKeyboardLifecycleTests(unittest.TestCase):
    def run_lifecycle(self, fixture):
        with use_config(fixture.main._runtime), track_testbeds() as beds:
            self.beds = beds
            with patch.object(ble_keyboard, "DUT", return_value=fixture.main), \
                 patch("amity_harness.ble_keyboard.time.sleep"):
                ble_keyboard.test_ble_keyboard_lifecycle()

    def checks(self):
        bed, = self.beds
        return next(row["checks"] for row in bed.report["control_observations"]
                    if row["kind"] == "ble_keyboard_lifecycle")

    def test_six_scans_share_advertisement_and_pair4_and6_each_deliver_physical_tap(self):
        fixture = LifecycleFixture()
        self.run_lifecycle(fixture)
        mutations = [path for path, data in fixture.calls if data is not None]
        self.assertEqual(mutations, ["/api/remotes/scan"] * 4 +
                         ["/api/remotes/pair", "/api/remotes/unpair"] +
                         ["/api/remotes/scan"] * 2 + ["/api/remotes/pair", "/api/remotes/unpair"])
        self.assertEqual(fixture.state["remote_generation"], "4")
        self.assertFalse(fixture.state["bonded"] or fixture.state["connected"])
        self.assertEqual(fixture.worker_calls.count(("open",)), 1)
        self.assertEqual(fixture.worker_calls.count(("forget_peer",)), 2)
        self.assertEqual(fixture.worker_calls.count(("close",)), 1)
        self.assertEqual([row for row in fixture.worker_calls if row[0] == "report" and row[1]],
                         [("report", (0x3C,), 0), ("report", (0x28,), 0)] * 2)
        self.assertEqual(fixture.readiness, [False, True, False, True])
        self.assertEqual(fixture.events, [("hdmi.scan", 4), ("activity.create", 4),
                         ("activity.start", 4), ("tap.down", 4), ("tap.up", 4), ("activity.standby", 4),
                         ("activity.start", 6), ("tap.down", 6), ("tap.up", 6), ("activity.standby", 6),
                         ("activity.remove", 6)])
        self.assertEqual([row["before"]["remote_generation"] for row in self.checks()],
                         ["0", "0", "0", "0", "2", "2"])
        self.assertEqual([row["paired"]["remote_generation"] for row in self.checks() if "paired" in row], ["1", "3"])
        self.assertTrue(all(row["passed"] for row in self.checks()))
        bed, = self.beds
        tap_checks = [check for row in bed.report["control_observations"] if row["kind"] == "ble_keyboard_input"
                      for check in row["checks"]]
        self.assertEqual(len(tap_checks), 2)
        self.assertTrue(all(check["passed"] and check["press_frame"] == "14:44:00"
                            and check["release_frame"] == "14:45" for check in tap_checks))
        self.assertIsNone(fixture.source._session)
        self.assertIsNone(fixture.main._testbed)
        fixture.main.press_fw_reset.assert_not_called()
        fixture.main.capture_logs.assert_not_called()

    def test_scan_miss_stops_immediately_without_pairing_or_hdmi_work(self):
        fixture = LifecycleFixture()
        original = fixture.main.request

        def request(path, data=None, **kwargs):
            if path == "/api/remotes/scan" and fixture.remote_scans == 2:
                fixture.omit_candidate = True
            return original(path, data, **kwargs)

        fixture.main.request = request
        with self.assertRaisesRegex(RuntimeError, "did not find this exact test keyboard"):
            self.run_lifecycle(fixture)
        self.assertEqual(fixture.remote_scans, 3)
        self.assertNotIn("/api/remotes/pair", [path for path, _ in fixture.calls])
        self.assertEqual(fixture.events, [])
        self.assertEqual(fixture.worker_calls[-1], ("close",))

    def test_missing_physical_key_after_pair4_fails_and_cleans_up_before_scan5(self):
        fixture = LifecycleFixture()
        fixture.missing_key = True
        with self.assertRaisesRegex(TimeoutError, "No fresh physical BLE key press"):
            self.run_lifecycle(fixture)
        self.assertEqual(fixture.remote_scans, 4)
        self.assertEqual(fixture.state["remote_generation"], "2")
        self.assertFalse(fixture.active or fixture.state["bonded"])
        self.assertIn(("activity.remove", 4), fixture.events)
        self.assertEqual(fixture.worker_calls[-1], ("close",))

    def test_boot_or_consortium_change_after_unpair4_stops_before_scan5(self):
        for field in ("boot_id", "consortium_id"):
            with self.subTest(field=field):
                fixture = LifecycleFixture()
                original = fixture.main.get_system

                def system():
                    state = original()
                    if ("forget_peer",) in fixture.worker_calls:
                        state[field] = "c" * 32
                    return state

                fixture.main.get_system = system
                with self.assertRaisesRegex(AssertionError, "main boot or consortium"):
                    self.run_lifecycle(fixture)
                self.assertEqual(fixture.remote_scans, 4)
                self.assertFalse(fixture.state["bonded"])
                self.assertEqual(fixture.worker_calls[-1], ("close",))

    def test_changed_generation_or_busy_state_after_unpair4_cannot_start_scan5(self):
        for changed in ("remote_generation", "scanning"):
            with self.subTest(changed=changed):
                fixture = LifecycleFixture()
                original = fixture.main.request

                def request(path, data=None, **kwargs):
                    state = original(path, data, **kwargs)
                    if path == "/api/remotes" and data is None and ("forget_peer",) in fixture.worker_calls:
                        state[changed] = "3" if changed == "remote_generation" else True
                    return state

                fixture.main.request = request
                with self.assertRaises(AssertionError):
                    self.run_lifecycle(fixture)
                self.assertEqual(fixture.remote_scans, 4)
                self.assertFalse(fixture.state["bonded"])

    def test_existing_user_bond_is_rejected_without_advertising_or_unpairing(self):
        fixture = LifecycleFixture()
        fixture.state["bonded"] = True
        with self.assertRaisesRegex(AssertionError, "idle and unbonded"):
            self.run_lifecycle(fixture)
        self.assertEqual(fixture.worker_calls, [])
        self.assertFalse(any(data is not None for _, data in fixture.calls))
        self.assertTrue(fixture.state["bonded"])

    def test_owned_unpair4_failure_stops_before_scan5_and_retains_failure(self):
        fixture = LifecycleFixture()
        original = fixture.main.request

        def request(path, data=None, **kwargs):
            if path == "/api/remotes/unpair" and data is not None:
                fixture.calls.append((path, deepcopy(data)))
                raise AmityHTTPError(409, "Synthetic unpair refusal")
            return original(path, data, **kwargs)

        fixture.main.request = request
        with self.assertRaises((AmityHTTPError, TestBedCleanupError)):
            self.run_lifecycle(fixture)
        self.assertEqual(fixture.remote_scans, 4)
        self.assertEqual(sum(path == "/api/remotes/pair" for path, _ in fixture.calls), 1)
        self.assertEqual(fixture.worker_calls[-1], ("close",))
        self.assertTrue(fixture.state["bonded"])
        self.assertFalse(fixture.active)
        self.assertIsNone(fixture.main._testbed)

    def test_ble_group_runs_lifecycle_immediately_after_standard_preparation(self):
        called = []

        def capture(test, config):
            called.append(test.__name__)
            return {"test": test.__name__, "passed": True, "testbeds": []}

        argv = ["run.py", "--test", "ble_keyboard", "--credentials", "/private/unused.json", "--no-status-led"]
        with patch("sys.argv", argv), patch("amity_harness.runner.run_test", side_effect=capture), \
             redirect_stdout(io.StringIO()), self.assertRaises(SystemExit) as exited:
            runpy.run_path(str(Path(__file__).resolve().parents[1] / "run.py"), run_name="__main__")
        self.assertEqual(exited.exception.code, 0)
        self.assertEqual(called, ["prepare_main", "test_ble_keyboard_lifecycle", "test_ble_keyboard_local",
                                  "test_ble_keyboard_remote", "reset_main"])


if __name__ == "__main__":
    unittest.main()
