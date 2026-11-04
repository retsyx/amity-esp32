"""Hostname edits require fresh shipping-UI proof and preserve fixture ownership."""

from copy import deepcopy
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import DUT
from amity_harness.config import RunConfig, use_config
from amity_harness.dut import AmityHTTPError
from amity_harness.hostname import HostnameChanges


class Clock:
    def __init__(self):
        self.now = 100.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class HostnameFixture:
    def __init__(self, *, member=True):
        self.clock = Clock()
        self.runtime = RunConfig(insecure=True, timeout=1, usb_reset=self.usb_reset)
        with use_config(self.runtime):
            self.main = DUT("amity-dut")
            self.secondary = DUT("amity-dut-2")
        self.group = "3" * 32
        self.owners = (self.main, self.secondary) if member else (self.main,)
        self.boots = {dut.identity.device_id: str(index + 1) * 32 for index, dut in enumerate(self.owners)}
        self.names = {dut.identity.device_id: dut.name for dut in self.owners}
        self.revisions = {owner: 7 for owner in self.boots}
        self.requests, self.callbacks, self.events, self.operations = [], [], [], {}
        self.system_changes, self.settings_changes = {}, {}
        self.health_changes = {}
        self.current_activity = -1
        self.config = {"version": 3, "consortium_id": self.group,
                       "controller_octopus_id": self.main.identity.device_id,
                       "config_revision": "4", "presentation_revision": "5",
                       "activities": [{"activity_id": "6" * 32, "label": "Watch"}],
                       "components": [{"component_id": "7" * 32, "label": "Player"}]}
        self.fail_post = self.lose_post = self.skip_usb_reboot = False
        self.commit_post = True
        self.next_operation_stage = "applied"
        self.fail_usb_after_reset = False
        self.operation_changes = {}
        self.main._token = "initial-token"
        self.main.request = self.request
        self.main.authenticate = self.authenticate
        self.main.probe_ready = self.probe_ready
        self.main.scan_hdmi = self.scan
        self.members = ()
        self.bed = SimpleNamespace(_entered=True, _closed=False, components=(object(),),
            _member_proofs={}, session=SimpleNamespace(assert_healthy=lambda: None),
            add_cleanup=self.callbacks.append)
        self.main._testbed = self.secondary._testbed = self.bed
        if member:
            row = SimpleNamespace(main=self.main, candidate=self.secondary, name=self.secondary.name,
                device_id=self.secondary.identity.device_id, _created=True, _closed=False,
                boot=self.boots[self.main.identity.device_id],
                member_boot=self.boots[self.secondary.identity.device_id],
                _initial_system={"boot_id": self.boots[self.main.identity.device_id],
                                 "membership": {"peers": []}})
            self.members = (row,)
            self.bed._member_proofs[row.device_id] = row
        self.activity_row = {"activity_id": "6" * 32, "name": "Watch"}
        self.activity = SimpleNamespace(dut=self.main, _created=True, _closed=False,
            activity_id=self.activity_row["activity_id"], _created_row=deepcopy(self.activity_row),
            boot=self.boots[self.main.identity.device_id], scan=None,
            refresh_scan=lambda scan: self.events.append(("activity.refresh", scan)),
            _action=self.activity_action)
        self.scans = []

    def authenticate(self):
        if not self.main._token:
            self.events.append(("authenticate", self.main.url))
            self.main._token = "fresh-token"

    def probe_ready(self, **kwargs):
        self.events.append(("probe", self.main.url))
        owner = self.main.identity.device_id
        return {"device_id": owner, "hostname": self.names[owner],
                "boot_id": self.boots[owner], "ready": True, **self.health_changes}

    def activity_action(self, kind, *, check_health):
        if kind != "standby" or check_health is not False:
            raise AssertionError("Cleanup must stop its exact owned activity")
        self.events.append(("activity.cleanup_standby",))
        self.current_activity = -1

    def system(self):
        main = self.main.identity.device_id
        peers = [{"octopus_id": dut.identity.device_id, "hostname": self.names[dut.identity.device_id],
                  "boot_id": self.boots[dut.identity.device_id], "enrolled": True, "ready": True}
                 for dut in self.owners if dut is not self.main]
        result = {"octopus_id": main, "consortium_id": self.group, "boot_id": self.boots[main],
                  "hostname": self.names[main], "mode": "coordinator", "ready": True,
                  "configuration_pending": False, "applied_config_revision": self.config["config_revision"],
                  "applied_presentation_revision": self.config["presentation_revision"],
                  "named_devices": [{"octopus_id": owner, "hostname": name} for owner, name in self.names.items()],
                  "membership": {"coordinator": True, "authority_ready": True, "member": False, "peers": peers}}
        result.update(deepcopy(self.system_changes))
        return result

    def settings(self, owner):
        result = {"boot_id": self.boots[owner], "hostname": self.names[owner],
                  "hostname_revision": str(self.revisions[owner]), "settings_revision": "8",
                  "adapters": [{"tentacle_id": 0, "configured": True,
                                "default_front_physical_address": "1.0.0.0"}]}
        result.update(deepcopy(self.settings_changes))
        return {"status": "ready", "error": "", "settings": result}

    def request(self, path, data=None, **kwargs):
        self.requests.append((path, deepcopy(data), self.main.url))
        if data is not None:
            if path == "/api/reboot":
                self.events.append(("http.reboot", self.main.name))
                self.boots[self.main.identity.device_id] = "a" * 32
                return {"ok": True}
            if not path.startswith("/api/devices/") or not path.endswith("/settings"):
                raise AssertionError("Unexpected mutation: " + path)
            if self.fail_post:
                raise AmityHTTPError(409, "settings rejected")
            owner = path.split("/")[3]
            if data["expected_boot_id"] != self.boots[owner] or data["expected_hostname_revision"] != str(self.revisions[owner]):
                raise AssertionError("Hostname POST omitted the reviewed boot/revision")
            if data["changes"] != []:
                raise AssertionError("Hostname test changed adapter addresses")
            if self.commit_post:
                self.names[owner] = data["hostname"]
                self.revisions[owner] += 1
            self.operations[data["request_id"]] = {"operation_id": data["request_id"], "stage": self.next_operation_stage,
                "boot_id": self.boots[self.main.identity.device_id], "rebooting": False}
            self.next_operation_stage = "applied"
            if self.lose_post:
                self.lose_post = False
                raise OSError("reply lost after commit")
            return dict(self.operations[data["request_id"]], stage="admitted")
        if path == "/api/system":
            return self.system()
        if path == "/api/config":
            return deepcopy(self.config)
        if path == "/api/activity-editor/status":
            return {"expected_boot_id": self.boots[self.main.identity.device_id],
                    "current_activity": self.current_activity, "activity_settling": False}
        if path == "/api/activity-editor":
            return {"activities": [deepcopy(self.activity_row)]}
        if path.startswith("/api/devices/") and path.endswith("/settings"):
            return self.settings(path.split("/")[3])
        if path.startswith("/api/operations/"):
            operation = deepcopy(self.operations[path.rsplit("/", 1)[1]])
            operation.update(self.operation_changes)
            return operation
        raise AssertionError("Unexpected shipping endpoint: " + path)

    def usb_reset(self, dut):
        if dut is self.main:
            raise AssertionError("Main should use its shipping HTTP restart")
        self.events.append(("usb.reset", dut.name))
        if not self.skip_usb_reboot:
            self.boots[dut.identity.device_id] = "b" * 32
        if self.fail_usb_after_reset:
            raise OSError("USB process failed after issuing reset")

    def scan(self, **kwargs):
        self.events.append(("scan",))
        fixture = self

        class Scan:
            def assert_contains(self, *components):
                if components != fixture.bed.components:
                    raise AssertionError("Scan lost the physical fixture")

            assert_contains_only = assert_contains

        result = Scan()
        self.scans.append(result)
        self.main.last_scan = result
        return result

    def helper(self):
        return HostnameChanges(self.main, members=self.members, activities=(self.activity,))

    def posts(self):
        return [(path, body, origin) for path, body, origin in self.requests if body is not None]


class HostnameTests(unittest.TestCase):
    def fixture(self, **kwargs):
        fixture = HostnameFixture(**kwargs)
        timer = patch("amity_harness.hostname.time", fixture.clock)
        timer.start()
        self.addCleanup(timer.stop)
        opener = patch("amity_harness.dut.HTTPSOpener", return_value=SimpleNamespace(close=lambda: None))
        opener.start()
        self.addCleanup(opener.stop)
        return fixture, fixture.helper()

    def test_main_rename_follows_new_origin_and_restores_canonical_name(self):
        fixture, names = self.fixture()
        before = dict(fixture.boots)
        names.rename(fixture.main, "amity-dut-t")
        names.assert_hostname(fixture.main, "amity-dut-t")
        self.assertEqual(fixture.boots, before)
        self.assertEqual(fixture.main.url, "https://amity-dut-t.local")
        self.assertEqual(fixture.runtime.admin_origins[fixture.main.name], fixture.main.url)
        self.assertIn(("authenticate", "https://amity-dut-t.local"), fixture.events)
        self.assertLess(fixture.events.index(("probe", "https://amity-dut-t.local")),
                        fixture.events.index(("authenticate", "https://amity-dut-t.local")))
        self.assertEqual(len(fixture.posts()), 1)
        path, body, origin = fixture.posts()[0]
        self.assertEqual(path, "/api/devices/" + fixture.main.identity.device_id + "/settings")
        self.assertEqual(origin, "https://amity-dut.local")
        self.assertEqual(body["expected_hostname_revision"], "7")
        names.close()
        self.assertEqual(fixture.names, {dut.identity.device_id: dut.name for dut in fixture.owners})
        self.assertEqual(fixture.main.url, "https://amity-dut.local")
        self.assertEqual(len(fixture.posts()), 2)
        self.assertFalse(any("factory-reset" in path for path, _, _ in fixture.requests))

    def test_member_rename_uses_only_the_main_http_session(self):
        fixture, names = self.fixture()
        original_origin = fixture.main.url
        names.rename(fixture.secondary, "amity-dut-2-t")
        self.assertEqual(fixture.members[0].name, "amity-dut-2-t")
        self.assertEqual(fixture.main.url, original_origin)
        self.assertIsNone(fixture.secondary._token)
        self.assertTrue(all(origin == original_origin for _, _, origin in fixture.requests))
        names.restore()
        self.assertEqual(fixture.members[0].name, "amity-dut-2")

    def test_new_origin_identity_is_checked_before_sending_credentials(self):
        for key, value in (("device_id", "f" * 32), ("boot_id", "e" * 32),
                           ("hostname", "another-name")):
            with self.subTest(key=key):
                fixture, names = self.fixture()
                fixture.health_changes[key] = value
                with self.assertRaisesRegex(AssertionError, "another device or boot"):
                    names.rename(fixture.main, "amity-dut-t")
                self.assertFalse(any(event[0] == "authenticate" for event in fixture.events))

    def test_committed_post_with_lost_reply_is_reconciled_without_replay(self):
        fixture, names = self.fixture()
        fixture.lose_post = True
        names.rename(fixture.main, "amity-dut-t")
        self.assertEqual(len(fixture.posts()), 1)
        self.assertEqual(fixture.names[fixture.main.identity.device_id], "amity-dut-t")
        self.assertIsNone(names._pending)

    def test_foreign_identity_or_group_is_rejected(self):
        for field in ("octopus_id", "consortium_id"):
            with self.subTest(field=field):
                fixture, names = self.fixture()
                fixture.system_changes[field] = "f" * 32
                with self.assertRaisesRegex(AssertionError, "original main or system identity"):
                    names.rename(fixture.main, "amity-dut-t")
                self.assertFalse(fixture.posts())

    def test_spontaneous_reboot_does_not_refresh_fences(self):
        fixture, names = self.fixture()
        prior = dict(names.boots)
        fixture.boots[fixture.secondary.identity.device_id] = "c" * 32
        with self.assertRaisesRegex(AssertionError, "without a requested reboot"):
            names.assert_hostname(fixture.main, fixture.main.name)
        self.assertEqual(names.boots, prior)
        self.assertFalse(fixture.scans)

    def test_configuration_changes_fail(self):
        fixture, names = self.fixture()
        fixture.config["activities"][0]["label"] = "Changed"
        with self.assertRaisesRegex(AssertionError, "saved equipment or activities"):
            names.assert_hostname(fixture.main, fixture.main.name)

    def test_hostname_testing_does_not_require_or_query_matter(self):
        fixture, names = self.fixture()
        names.rename(fixture.main, "amity-dut-t")
        names.reboot(fixture.main)
        names.close()
        self.assertFalse(any("matter" in path for path, _, _ in fixture.requests))

    def test_saved_adapter_changes_fail_even_if_hostname_is_correct(self):
        fixture, names = self.fixture()
        fixture.settings_changes["settings_revision"] = "9"
        with self.assertRaisesRegex(AssertionError, "saved device settings"):
            names.assert_hostname(fixture.secondary, fixture.secondary.name)

    def test_wrong_operation_identity_or_terminal_stage_is_rejected(self):
        for changes in ({"operation_id": "f" * 32}, {"boot_id": "f" * 32},
                        {"stage": "failed"}, {"rebooting": True}):
            with self.subTest(changes=changes):
                fixture, names = self.fixture()
                fixture.operation_changes = changes
                with self.assertRaisesRegex(AssertionError, "operation changed|confirmed applied"):
                    names.rename(fixture.secondary, "amity-dut-2-t")
                self.assertEqual(len(fixture.posts()), 1)

    def test_original_failure_leaves_changed_hostname_for_debugging(self):
        fixture, names = self.fixture()
        with self.assertRaisesRegex(AssertionError, "activity wire failure"):
            with names:
                names.rename(fixture.main, "amity-dut-t")
                raise AssertionError("activity wire failure")
        self.assertEqual(fixture.names[fixture.main.identity.device_id], "amity-dut-t")
        self.assertEqual(len(fixture.posts()), 1)
        self.assertFalse(names._closed)

    def test_failure_with_owned_activity_active_leaves_it_active(self):
        fixture, names = self.fixture()
        with self.assertRaisesRegex(AssertionError, "missing fresh command"):
            with names:
                names.rename(fixture.main, "amity-dut-t")
                fixture.current_activity = 0
                raise AssertionError("missing fresh command")
        self.assertEqual(fixture.current_activity, 0)
        self.assertNotIn(("activity.cleanup_standby",), fixture.events)
        self.assertEqual(fixture.names[fixture.main.identity.device_id], "amity-dut-t")
        self.assertFalse(names._closed)

    def test_successful_close_requires_standby_without_stopping_activity(self):
        fixture, names = self.fixture()
        names.rename(fixture.main, "amity-dut-t")
        fixture.current_activity = 0
        fixture.activity_row["activity_id"] = "f" * 32
        with self.assertRaisesRegex(AssertionError, "begin in standby"):
            names.close()
        self.assertFalse(names._closed)
        self.assertFalse(any(event[0] == "activity.cleanup_standby" for event in fixture.events))

    def test_failed_restoration_stops_without_touching_remaining_owners(self):
        fixture, names = self.fixture()
        names.rename(fixture.main, "amity-dut-t")
        names.rename(fixture.secondary, "amity-dut-2-t")
        fixture.fail_post = True
        with self.assertRaises(AmityHTTPError):
            names.close()
        self.assertFalse(names._closed)
        self.assertEqual(fixture.names[fixture.secondary.identity.device_id], "amity-dut-2-t")
        self.assertEqual(fixture.names[fixture.main.identity.device_id], "amity-dut-t")
        self.assertEqual(len(fixture.posts()), 3)

    def test_terminal_failed_rename_leaves_the_observed_state_unchanged(self):
        for target in ("main", "secondary"):
            for committed in (True, False):
                with self.subTest(target=target, committed=committed):
                    fixture, names = self.fixture()
                    dut = getattr(fixture, target)
                    fixture.commit_post = committed
                    fixture.next_operation_stage = "failed"
                    with self.assertRaises((AssertionError, TimeoutError)):
                        with names:
                            names.rename(dut, dut.name + "-t")
                    self.assertEqual(fixture.names[dut.identity.device_id], dut.name + "-t" if committed else dut.name)
                    self.assertEqual(len(fixture.posts()), 1)
                    self.assertIsNotNone(names._pending)
                    self.assertFalse(any("factory-reset" in path for path, _, _ in fixture.requests))

    def test_failed_secondary_reset_leaves_both_boards_for_debugging(self):
        fixture, names = self.fixture()
        names.rename(fixture.secondary, "amity-dut-2-t")
        before_main = names.boots[fixture.main.identity.device_id]
        fixture.fail_usb_after_reset = True
        with self.assertRaisesRegex(OSError, "after issuing reset"):
            with names:
                names.reboot(fixture.main, fixture.secondary)
        self.assertFalse(any(event[0] == "http.reboot" for event in fixture.events))
        self.assertEqual(names.boots[fixture.main.identity.device_id], before_main)
        self.assertEqual(fixture.names[fixture.secondary.identity.device_id], "amity-dut-2-t")
        self.assertIsNotNone(names._rebooting)
        self.assertEqual(len(fixture.posts()), 1)

    def test_main_member_and_both_reboot_refresh_only_expected_boots(self):
        for targets in (("main",), ("secondary",), ("main", "secondary")):
            with self.subTest(targets=targets):
                fixture, names = self.fixture()
                prior = dict(names.boots)
                duts = tuple(getattr(fixture, target) for target in targets)
                names.reboot(*duts)
                for dut in fixture.owners:
                    if dut in duts:
                        self.assertNotEqual(names.boots[dut.identity.device_id], prior[dut.identity.device_id])
                    else:
                        self.assertEqual(names.boots[dut.identity.device_id], prior[dut.identity.device_id])
                self.assertEqual(fixture.members[0].boot, names.boots[fixture.main.identity.device_id])
                self.assertEqual(fixture.members[0].member_boot, names.boots[fixture.secondary.identity.device_id])
                self.assertEqual(fixture.activity.boot, names.boots[fixture.main.identity.device_id])
                self.assertIs(fixture.activity.scan, fixture.scans[0])
                self.assertIn(("activity.refresh", fixture.scans[0]), fixture.events)
                self.assertTrue(any(check["check"] == "reboot_verified" for check in names.report["checks"]))
                self.assertIsNone(names._rebooting)
                if len(duts) == 2:
                    self.assertLess(fixture.events.index(("usb.reset", fixture.secondary.name)),
                                    fixture.events.index(("http.reboot", fixture.main.name)))

    def test_successful_reset_process_without_new_boot_is_a_failure(self):
        fixture, names = self.fixture()
        old_boot = names.boots[fixture.secondary.identity.device_id]
        fixture.skip_usb_reboot = True
        with self.assertRaises(TimeoutError):
            names.reboot(fixture.secondary)
        self.assertEqual(names.boots[fixture.secondary.identity.device_id], old_boot)
        self.assertFalse(fixture.scans)

    def test_reboot_rejects_changed_saved_activity_before_refresh(self):
        fixture, names = self.fixture()
        fixture.activity_row["name"] = "Edited by someone else"
        with self.assertRaisesRegex(AssertionError, "saved activity changed across reboot"):
            names.reboot(fixture.main)
        self.assertFalse(any(event[0] == "activity.refresh" for event in fixture.events))

    def test_primary_only_case_needs_no_member_or_usb_reset(self):
        fixture, names = self.fixture(member=False)
        names.rename(fixture.main, "amity-dut-t")
        names.reboot(fixture.main)
        names.close()
        self.assertFalse(any(event[0] == "usb.reset" for event in fixture.events))


if __name__ == "__main__":
    unittest.main()
