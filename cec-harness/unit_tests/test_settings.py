"""Shipping-UI adapter setting changes retain exact identity and boot fences."""

from copy import deepcopy
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness.dut import AmityHTTPError
from amity_harness.membership import MembershipConflict
from amity_harness.settings import FrontPhysicalAddress
from unit_tests.test_membership import Fixture


class SettingsFixture(Fixture):
    def __init__(self):
        super().__init__()
        self.capacity = 4
        self.defaults = {index: "1.0.0.0" for index in range(4)}
        self.selected = dict(self.defaults)
        self.revision = 7
        self.writes = []
        self.restarts = 0
        self.current_activity = -1
        self.bad_operation_id = self.bad_main_boot = self.reject_settings = False
        self.lose_settings_reply = self.restart_during_save = self.change_other_default = False
        self.post_save_unready_reads = 0
        self.pending_address_reads = 0
        self.observed_address = None
        self.effective_override = None

    @property
    def settings_path(self):
        return "/api/devices/" + self.candidate.identity.device_id + "/settings"

    def request(self, path, data=None, **kwargs):
        if path == "/api/activity-editor/status" and data is None:
            return {"current_activity": self.current_activity, "activity_settling": False,
                    "expected_boot_id": self.boot}
        if path != self.settings_path:
            return super().request(path, data, **kwargs)
        self.calls.append((path, deepcopy(data)))
        if data is None:
            result = {"status": "ready", "error": "", "settings": {
                "boot_id": self.settings_boot, "settings_revision": str(self.revision),
                "adapters": [{"tentacle_id": index, "configured": True,
                              "default_front_physical_address": address,
                              "observed_front_physical_address": None,
                              "effective_front_physical_address": self.selected[index]}
                             for index, address in self.defaults.items()]}}
            if self.observed_address is not None:
                result["settings"]["adapters"][2]["observed_front_physical_address"] = self.observed_address
            if self.effective_override is not None:
                result["settings"]["adapters"][2]["effective_front_physical_address"] = self.effective_override
            if self.pending_address_reads:
                self.pending_address_reads -= 1
                result["settings"]["adapters"][2].update(effective_front_physical_address=None)
            return result
        self.writes.append(deepcopy(data))
        if self.reject_settings:
            raise AmityHTTPError(409, "Changed settings review")
        if (data["expected_boot_id"] != self.settings_boot
                or data["expected_settings_revision"] != str(self.revision)):
            raise AssertionError("A settings write must use its fresh reviewed boot and revision")
        for row in data["changes"]:
            index = row["tentacle_id"]
            self.defaults[index] = row["default_front_physical_address"]
            self.selected[index] = self.observed_address if index == 2 and self.observed_address is not None else self.defaults[index]
        if self.change_other_default:
            self.defaults[0] = "3.0.0.0"
        self.revision += 1
        self.pending_address_reads = self.post_save_unready_reads
        if self.restart_during_save:
            self.restarts += 1
            self.settings_boot = str(self.restarts) * 32
            peer, = (peer for peer in self.system["membership"]["peers"]
                     if peer["octopus_id"] == self.candidate.identity.device_id)
            peer["boot_id"] = self.settings_boot
        result = self.operation(data["request_id"], "applied")
        result["rebooting"] = False
        if self.bad_operation_id:
            result["operation_id"] = "9" * 32
        if self.bad_main_boot:
            result["boot_id"] = "8" * 32
        self.operations[data["request_id"]] = result
        if self.lose_settings_reply:
            self.lose_settings_reply = False
            raise OSError("The settings write reply was lost")
        admitted = deepcopy(result)
        admitted["stage"], admitted["rebooting"] = "admitted", False
        return admitted


class AdapterSettingsTests(unittest.TestCase):
    def setUp(self):
        sleeper = patch("amity_harness.settings.time.sleep")
        sleeper.start()
        self.addCleanup(sleeper.stop)

    def fixture(self):
        fixture = SettingsFixture()
        member = fixture.added(timeout=0.2).create()
        return fixture, member

    def test_ui_write_and_restore_preserve_boot_and_keep_other_defaults(self):
        fixture, member = self.fixture()
        old_boot = member.member_boot
        with member.front_physical_address(2, "1.2.0.0"):
            self.assertEqual(member.member_boot, old_boot)
            self.assertIs(fixture.bed._member_proofs[member.device_id], member)
            self.assertEqual(fixture.defaults, {0: "1.0.0.0", 1: "1.0.0.0", 2: "1.2.0.0", 3: "1.0.0.0"})
            selected_boot = member.member_boot
        self.assertEqual(member.member_boot, selected_boot)
        self.assertEqual(fixture.restarts, 0)
        self.assertEqual(fixture.defaults, {index: "1.0.0.0" for index in range(4)})
        self.assertEqual(len(fixture.writes), 2)
        first, restore = fixture.writes
        self.assertEqual(set(first), {"request_id", "expected_boot_id", "expected_settings_revision", "changes"})
        self.assertEqual(first["expected_boot_id"], old_boot)
        self.assertEqual(first["expected_settings_revision"], "7")
        self.assertEqual(first["changes"], [{"tentacle_id": 2, "default_front_physical_address": "1.2.0.0"}])
        self.assertEqual(restore["expected_boot_id"], selected_boot)
        self.assertEqual(restore["expected_settings_revision"], "8")
        self.assertEqual(restore["changes"][0]["default_front_physical_address"], "1.0.0.0")
        self.assertEqual([row["phase"] for row in fixture.main.adapter_settings_operations], ["apply", "restore"])
        self.assertTrue(all(row["stage"] == "applied" and row["member_boot"] for row in fixture.main.adapter_settings_operations))
        member.close()
        self.assertEqual(fixture.posts("remove")[0]["expected_target_boot_id"], member.member_boot)

    def test_equal_saved_and_effective_address_needs_no_write_or_restart(self):
        fixture, member = self.fixture()
        with member.front_physical_address(2, "1.0.0.0"):
            self.assertEqual(fixture.writes, [])
        self.assertEqual(fixture.restarts, 0)

    def test_applied_selected_address_must_be_available(self):
        fixture, member = self.fixture()
        fixture.post_save_unready_reads = 2
        with member.front_physical_address(2, "1.2.0.0"):
            self.assertEqual(fixture.pending_address_reads, 0)
            self.assertEqual(fixture.defaults[2], "1.2.0.0")
        self.assertEqual(fixture.pending_address_reads, 0)

    def test_restore_previous_default_keeps_live_observed_address_effective(self):
        fixture, member = self.fixture()
        fixture.observed_address = "1.2.0.0"
        fixture.selected[2] = fixture.observed_address
        with member.front_physical_address(2, "1.2.0.0"):
            self.assertEqual(fixture.defaults[2], "1.2.0.0")
        self.assertEqual(fixture.defaults[2], "1.0.0.0")
        settings = fixture.request(fixture.settings_path)["settings"]
        adapter, = (row for row in settings["adapters"] if row["tentacle_id"] == 2)
        self.assertEqual(adapter["default_front_physical_address"], "1.0.0.0")
        self.assertEqual(adapter["observed_front_physical_address"], "1.2.0.0")
        self.assertEqual(adapter["effective_front_physical_address"], "1.2.0.0")
        self.assertEqual(fixture.restarts, 0)

    def test_wrong_selected_address_is_rejected_even_when_default_was_saved(self):
        fixture, member = self.fixture()
        original = fixture.request
        def wrong_after_save(path, data=None, **kwargs):
            if path == fixture.settings_path and data is None and fixture.writes:
                fixture.effective_override = "4.0.0.0"
            return original(path, data, **kwargs)
        fixture.main.request = wrong_after_save
        with self.assertRaisesRegex(MembershipConflict, "scanned address or default"):
            member.front_physical_address(2, "1.2.0.0")

    def test_detected_address_equal_to_old_default_is_not_replaced(self):
        fixture, member = self.fixture()
        fixture.observed_address = "1.0.0.0"
        fixture.selected[2] = fixture.observed_address
        with member.front_physical_address(2, "1.2.0.0"):
            selected = fixture.request(fixture.settings_path)["settings"]["adapters"][2]
            self.assertEqual(selected["default_front_physical_address"], "1.2.0.0")
            self.assertEqual(selected["effective_front_physical_address"], "1.0.0.0")
        self.assertEqual(fixture.restarts, 0)

    def test_later_scan_keeps_selection_until_save_then_supplies_selected_address(self):
        fixture, member = self.fixture()
        fixture.observed_address = "4.0.0.0"
        self.assertEqual(fixture.request(fixture.settings_path)["settings"]["adapters"][2][
            "effective_front_physical_address"], "1.0.0.0")
        with member.front_physical_address(2, "1.2.0.0"):
            selected = fixture.request(fixture.settings_path)["settings"]["adapters"][2]
            self.assertEqual(selected["observed_front_physical_address"], "4.0.0.0")
            self.assertEqual(selected["effective_front_physical_address"], "4.0.0.0")
        self.assertEqual(fixture.restarts, 0)

    def test_equal_saved_default_with_different_detected_selection_needs_no_write(self):
        fixture, member = self.fixture()
        fixture.observed_address = "4.0.0.0"
        fixture.selected[2] = fixture.observed_address
        with member.front_physical_address(2, "1.0.0.0"):
            self.assertEqual(fixture.writes, [])

    def test_scan_loses_address_then_next_save_selects_new_default(self):
        fixture, member = self.fixture()
        fixture.selected[2] = "4.0.0.0"
        self.assertIsNone(fixture.observed_address)
        with member.front_physical_address(2, "1.2.0.0"):
            self.assertEqual(fixture.selected[2], "1.2.0.0")
        self.assertEqual(fixture.selected[2], "1.0.0.0")
        self.assertEqual(fixture.restarts, 0)

    def test_wrong_operation_or_main_boot_cannot_replace_member_proof(self):
        for field in ("bad_operation_id", "bad_main_boot"):
            with self.subTest(field=field):
                fixture, member = self.fixture()
                old_boot = member.member_boot
                setattr(fixture, field, True)
                with self.assertRaises(MembershipConflict):
                    member.front_physical_address(2, "1.2.0.0")
                self.assertEqual(member.member_boot, old_boot)

    def test_same_boot_and_unchanged_other_defaults_are_required(self):
        fixture, member = self.fixture()
        fixture.restart_during_save = True
        with self.assertRaisesRegex(MembershipConflict, "restarted"):
            member.front_physical_address(2, "1.2.0.0", timeout=0.01)
        fixture, member = self.fixture()
        fixture.change_other_default = True
        with self.assertRaisesRegex(MembershipConflict, "exactly the reviewed"):
            member.front_physical_address(2, "1.2.0.0")

    def test_lost_post_reply_is_not_repeated_and_cleanup_restores_after_proof(self):
        fixture, member = self.fixture()
        fixture.lose_settings_reply = True
        setting = FrontPhysicalAddress(member, 2, "1.2.0.0")
        with self.assertRaises(OSError):
            setting.apply()
        setting.close()
        self.assertEqual(len(fixture.writes), 2)
        self.assertEqual(fixture.defaults[2], "1.0.0.0")
        self.assertNotEqual(fixture.writes[0]["request_id"], fixture.writes[1]["request_id"])

    def test_cleanup_rejects_changed_settings_instead_of_overwriting_them(self):
        fixture, member = self.fixture()
        setting = member.front_physical_address(2, "1.2.0.0")
        fixture.defaults[2] = "4.0.0.0"
        with self.assertRaisesRegex(MembershipConflict, "changed before fixture cleanup"):
            setting.close()
        self.assertEqual(len(fixture.writes), 1)

    def test_rejected_write_does_not_restart_or_create_cleanup_write(self):
        fixture, member = self.fixture()
        fixture.reject_settings = True
        setting = FrontPhysicalAddress(member, 2, "1.2.0.0")
        with self.assertRaises(AmityHTTPError):
            setting.apply()
        setting.close()
        self.assertEqual(len(fixture.writes), 1)
        self.assertEqual(fixture.restarts, 0)

    def test_settings_writes_require_idle_main_and_configured_adapter(self):
        fixture, member = self.fixture()
        fixture.current_activity = 0
        with self.assertRaisesRegex(MembershipConflict, "idle"):
            member.front_physical_address(2, "1.2.0.0")
        self.assertEqual(fixture.writes, [])
        fixture, member = self.fixture()
        del fixture.defaults[2]
        with self.assertRaises((MembershipConflict, TimeoutError)):
            member.front_physical_address(2, "1.2.0.0", timeout=0.01)
        self.assertEqual(fixture.writes, [])

    def test_configured_quiet_adapter_needs_no_presence_sensor(self):
        fixture, member = self.fixture()
        with member.front_physical_address(2, "1.2.0.0"):
            self.assertEqual(fixture.defaults[2], "1.2.0.0")
        self.assertEqual(fixture.defaults[2], "1.0.0.0")
        self.assertTrue(all(set(row) == {"tentacle_id", "default_front_physical_address"}
                            for write in fixture.writes for row in write["changes"]))

    def test_body_failure_survives_restore_failure_and_cleanup_can_retry(self):
        fixture, member = self.fixture()
        setting = member.front_physical_address(2, "1.2.0.0")
        failure = AssertionError("original physical assertion")
        with patch.object(setting, "close", side_effect=OSError("restore response lost")):
            with self.assertRaises(AssertionError) as raised:
                with setting:
                    raise failure
        self.assertIs(raised.exception, failure)
        self.assertTrue(any("registered TestBed cleanup" in note for note in failure.__notes__))
        setting.close()
        self.assertEqual(fixture.defaults[2], "1.0.0.0")

    def test_restore_failure_without_body_failure_still_fails(self):
        _, member = self.fixture()
        setting = member.front_physical_address(2, "1.2.0.0")
        with patch.object(setting, "close", side_effect=OSError("restore response lost")):
            with self.assertRaisesRegex(OSError, "restore response lost"):
                with setting:
                    pass
        setting.close()


if __name__ == "__main__":
    unittest.main()
