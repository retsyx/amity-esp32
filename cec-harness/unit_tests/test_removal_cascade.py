"""Removal must use a fresh review and atomically prune only target-owned data."""

from copy import deepcopy
import json
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness.dut import AmityHTTPError
from amity_harness.membership import MembershipConflict
from amity_harness.testbed import TestBed
import test_membership as membership_fixture
import test_testbed as bed_fixture


class CascadeFixture(membership_fixture.Fixture):
    """Model the shipping review/Remove transaction, without GPIO or network I/O."""

    roles = ("source", "display", "audio", "switch_device")

    def __init__(self):
        super().__init__()
        self.bed._activities = []
        self.main.activity_operations = []
        self.main.get_configuration = self.editor
        self.main.get_state = self.state
        self.target_settings_revision = "1"
        self.main_settings_revision = "2"
        self.settings_reads_pending = 0
        self.reading_effect = None
        self.settings_purged = False
        self.keep_settings = self.keep_name = self.keep_peer = False
        self.stale_code = "stale_removal"
        self.stale_effect = self.after_remove = None
        self.accept_stale = False
        self.remove_http_error = None
        self.missing_operation_reads = 0
        self.member = self.added().create()
        self.resources = []
        owner = self.candidate.identity.device_id
        for identity, kind in (("4" * 32, "source"), ("5" * 32, "display"), ("6" * 32, "receiver")):
            self.config["components"].append({
                "component_id": identity, "label": "Remote " + kind,
                "generated_label": "Remote " + kind, "label_override": False, "kind": kind,
                "qualified_names": [{"consortium_id": self.group, "octopus_id": owner,
                                     "tentacle_id": 0, "side": "front" if kind == "display" else "back",
                                     "osd_name": "Remote " + kind}]})
        selected = {"source": "4" * 32, "display": "5" * 32,
                    "audio": "6" * 32, "switch_device": "6" * 32}
        for number, role in enumerate(self.roles, 7):
            identity = format(number, "x") * 32
            row = {"activity_id": identity, "zone_id": self.config["primary_zone_id"],
                   "label": "Uses " + role, "generated_label": "Uses " + role,
                   "label_override": False, "source": None, "display": None,
                   "audio": None, "switch_device": None, "switch_input": 0}
            row[role] = selected[role]
            if role == "switch_device":
                row["switch_input"] = 4
            self.config["activities"].append(row)
            self.config["activity_order"].append(identity)
            self.config["zones"][0]["activity_order"].append(identity)
            self.config["external_slots"].append({"slot": number - 6, "activity_id": identity,
                                                  "tombstone": False, "hidden": True,
                                                  "alias": "Remote shortcut " + role})
            resource = SimpleNamespace(dut=self.main, boot=self.boot, _testbed=self.bed,
                                       _created=True, _closed=False, _needs_standby=False,
                                       activity_id=identity, _created_row=self.editor_row(row))
            created = selected[role] if role != "switch_device" else None
            resource._new_equipment = {record["component_id"]: deepcopy(record)
                                       for record in self.config["components"] if record["component_id"] == created}
            resource._allocated_equipment = set(resource._new_equipment)
            self.resources.append(resource)
            self.bed._activities.append(resource)
            self.main.activity_operations.append({"phase": "create", "activity_id": identity,
                                                   "operation": {"stage": "applied"}})
        self.advance()

    @staticmethod
    def editor_row(row):
        value = {"activity_id": row["activity_id"], "name": row["label"],
                 **{role: "component:" + row[role] if row.get(role) else ""
                    for role in ("source", "display", "audio")}}
        if row.get("switch_device"):
            value["switch"] = {"device": "component:" + row["switch_device"],
                               "input": row["switch_input"]}
        return value

    def editor(self):
        return {"version": 1, "expected_config_revision": self.config["config_revision"],
                "expected_presentation_revision": self.config["presentation_revision"],
                "activities": [self.editor_row(row) for row in self.config["activities"]],
                "device_choices": []}

    def state(self):
        return {"expected_boot_id": self.system["boot_id"], "current_activity": -1,
                "activity_settling": False, "activity_error": "",
                "expected_config_revision": self.config["config_revision"],
                "expected_presentation_revision": self.config["presentation_revision"]}

    def advance(self):
        self.bump()
        self.config["presentation_revision"] = str(int(self.config["presentation_revision"]) + 1)
        self.system["applied_presentation_revision"] = self.config["presentation_revision"]

    def add_unrelated(self):
        row = {"activity_id": "b" * 32, "zone_id": self.config["primary_zone_id"],
               "label": "Unrelated later activity", "generated_label": "Unrelated later activity",
               "label_override": False, "source": "3" * 32, "display": None,
               "audio": None, "switch_device": None, "switch_input": 0}
        self.config["activities"].append(row)
        self.config["activity_order"].append(row["activity_id"])
        self.config["zones"][0]["activity_order"].append(row["activity_id"])
        self.config["external_slots"].append({"slot": 5, "activity_id": row["activity_id"],
                                              "tombstone": False, "hidden": True,
                                              "alias": "Unrelated later shortcut"})
        self.bed._activities.append(SimpleNamespace(
            dut=self.main, boot=self.boot, _testbed=self.bed, _created=True, _closed=False,
            _needs_standby=False, activity_id=row["activity_id"], _created_row=self.editor_row(row),
            _new_equipment={}, _allocated_equipment=set()))
        self.main.activity_operations.append({"phase": "create", "activity_id": row["activity_id"],
                                               "operation": {"stage": "applied"}})
        self.advance()

    def request(self, path, data=None, **kwargs):
        if data is None and path.startswith("/api/operations/") and self.missing_operation_reads:
            self.calls.append((path, None))
            self.missing_operation_reads -= 1
            raise AmityHTTPError(404, "Unknown operation")
        if data is None and path == "/api/backup":
            self.calls.append((path, None))
            return deepcopy(self.config)
        if data is None and path == "/api/activity-editor":
            self.calls.append((path, None))
            return self.editor()
        if data is None and path == "/api/activity-editor/status":
            self.calls.append((path, None))
            return self.state()
        if data is None and path == "/api/devices/" + self.main.identity.device_id + "/settings":
            self.calls.append((path, None))
            return {"status": "ready", "error": "",
                    "settings": {"boot_id": self.boot, "settings_revision": self.main_settings_revision,
                                 "adapters": [{"tentacle_id": index, "name": "Main adapter " + str(index + 1),
                                               "configured": True, "default_front_physical_address": "1.0.0.0",
                                               "observed_front_physical_address": None,
                                               "effective_front_physical_address": None}
                                              for index in range(self.capacity)]}}
        if data is None and path == "/api/devices/" + self.candidate.identity.device_id + "/settings":
            if self.settings_purged and not self.keep_settings:
                self.calls.append((path, None))
                return {"status": "offline", "settings": None,
                        "error": "The device is offline. Saved details cannot be edited."}
            result = super().request(path, data, **kwargs)
            result["settings"]["settings_revision"] = self.target_settings_revision
            if self.settings_reads_pending:
                self.settings_reads_pending -= 1
                result["status"] = "reading"
                if self.reading_effect:
                    effect, self.reading_effect = self.reading_effect, None
                    effect(self)
            return result
        if path != "/api/membership/remove" or data is None:
            return super().request(path, data, **kwargs)
        self.calls.append((path, deepcopy(data)))
        owner = self.candidate.identity.device_id
        peer = next(row for row in self.system["membership"]["peers"] if row["octopus_id"] == owner)
        equipment = {row["component_id"] for row in self.config["components"]
                     if any(name["octopus_id"] == owner for name in row["qualified_names"])}
        affected = {row["activity_id"] for row in self.config["activities"]
                    if any(row.get(role) in equipment for role in self.roles)}
        stale = (data.get("octopus_id") != owner or data.get("confirm") is not True
                 or data.get("expected_target_boot_id") != peer["boot_id"]
                 or data.get("expected_membership_revision") != self.system["membership"]["membership_revision"]
                 or data.get("expected_config_revision") != self.config["config_revision"]
                 or data.get("expected_presentation_revision") != self.config["presentation_revision"]
                 or set(data.get("affected_activity_ids", [])) != affected
                 or len(data.get("affected_activity_ids", [])) != len(affected))
        if stale and not self.accept_stale:
            if self.stale_effect:
                self.stale_effect(self)
            error = AmityHTTPError(409, "The device or its activities changed")
            error.code = self.stale_code
            raise error
        if not self.keep_peer:
            self.system["membership"]["peers"] = [row for row in self.system["membership"]["peers"]
                                                    if row["octopus_id"] != owner]
        self.system["membership"]["membership_revision"] = str(int(self.system["membership"]["membership_revision"]) + 1)
        if not self.keep_name:
            self.system["named_devices"] = [row for row in self.system["named_devices"] if row["octopus_id"] != owner]
        self.config["octopuses"] = [row for row in self.config["octopuses"] if row["octopus_id"] != owner]
        self.config["components"] = [row for row in self.config["components"] if row["component_id"] not in equipment]
        self.config["activities"] = [row for row in self.config["activities"] if row["activity_id"] not in affected]
        self.config["activity_order"] = [identity for identity in self.config["activity_order"] if identity not in affected]
        for zone in self.config["zones"]:
            zone["activity_order"] = [identity for identity in zone["activity_order"] if identity not in affected]
        for slot in self.config["external_slots"]:
            if slot["activity_id"] in affected:
                slot.update(tombstone=True, hidden=False, alias="")
        self.settings_purged = True
        self.advance()
        if self.after_remove:
            self.after_remove(self)
        request_id = data["request_id"]
        self.operations[request_id] = self.operation(request_id, "complete", reset=self.reset_mode)
        if self.remove_http_error is not None:
            status, self.remove_http_error = self.remove_http_error, None
            raise AmityHTTPError(status, "The admitted response could not be sent")
        if self.lose_remove_reply:
            self.lose_remove_reply = False
            raise OSError("Removal response was lost after admission")
        return self.operation(request_id, "admitted")

    def finish_resources(self):
        for resource in self.bed._activities:
            resource._closed = True
            self.main.activity_operations.append({"phase": "cleanup", "activity_id": resource.activity_id,
                                                   "removed_activity": True, "retained_equipment": []})
        if any(row["activity_id"] == "b" * 32 for row in self.config["activities"]):
            identity = "b" * 32
            self.config["activities"] = [row for row in self.config["activities"] if row["activity_id"] != identity]
            self.config["activity_order"] = [value for value in self.config["activity_order"] if value != identity]
            for zone in self.config["zones"]:
                zone["activity_order"] = [value for value in zone["activity_order"] if value != identity]
            for slot in self.config["external_slots"]:
                if slot["activity_id"] == identity:
                    slot.update(tombstone=True, hidden=False, alias="")
            self.advance()


class RemovalCascadeTests(unittest.TestCase):
    def setUp(self):
        sleeper = patch("amity_harness.membership.time.sleep")
        sleeper.start()
        self.addCleanup(sleeper.stop)

    def fixture(self):
        return CascadeFixture()

    def test_stale_review_preserves_new_unrelated_data_then_fresh_review_cascades_all_four_roles(self):
        f = self.fixture()
        old = f.member.review_removal(*f.resources)
        f.add_unrelated()
        before = deepcopy(f.config)
        peer_before = deepcopy(f.system["membership"])
        old.assert_stale_rejected()
        self.assertEqual(f.config, before)
        self.assertEqual(f.system["membership"], peer_before)
        self.assertFalse(f.member._closed)
        fresh = f.member.review_removal(*f.resources)
        fresh.confirm()
        self.assertTrue(f.member._closed)
        self.assertEqual([row["activity_id"] for row in f.config["activities"]], ["2" * 32, "b" * 32])
        self.assertEqual(f.config["external_slots"][0], f.original["external_slots"][0])
        self.assertEqual(f.config["external_slots"][-1], before["external_slots"][-1])
        self.assertTrue(all(row["tombstone"] and not row["hidden"] and row["alias"] == ""
                            for row in f.config["external_slots"] if row["activity_id"] in {r.activity_id for r in f.resources}))
        posted = f.posts("remove")
        self.assertEqual(len(posted), 2)
        self.assertNotEqual(posted[0]["request_id"], posted[1]["request_id"])
        self.assertEqual(set(posted[1]["affected_activity_ids"]), {r.activity_id for r in f.resources})
        f.member.close()
        f.member.close()
        self.assertEqual(len(f.posts("remove")), 2)

    def test_stale_confirmation_requires_actual_review_change_and_correct_error_reason(self):
        f = self.fixture()
        review = f.member.review_removal(*f.resources)
        with self.assertRaises((MembershipConflict, AssertionError, ValueError)):
            review.assert_stale_rejected()
        self.assertEqual(f.posts("remove"), [])
        review = f.member.review_removal(*f.resources)
        f.add_unrelated()
        f.stale_code = "capacity"
        with self.assertRaises((MembershipConflict, AssertionError)):
            review.assert_stale_rejected()
        self.assertFalse(f.member._closed)

    def test_rejected_review_cannot_mutate_configuration_membership_or_settings(self):
        mutations = {
            "configuration": lambda f: f.config["activities"][0].update(label="Damaged unrelated activity"),
            "membership": lambda f: f.system["membership"]["peers"].pop(),
            "settings": lambda f: setattr(f, "target_settings_revision", "99"),
            "main_settings": lambda f: setattr(f, "main_settings_revision", "99"),
        }
        for description, mutation in mutations.items():
            with self.subTest(description=description):
                f = self.fixture()
                review = f.member.review_removal(*f.resources)
                f.add_unrelated()
                f.stale_effect = mutation
                with self.assertRaises((MembershipConflict, AssertionError)):
                    review.assert_stale_rejected()
                self.assertFalse(f.member._closed)

    def test_incorrectly_accepted_stale_confirmation_keeps_original_operation_for_cleanup(self):
        f = self.fixture()
        review = f.member.review_removal(*f.resources)
        f.add_unrelated()
        f.accept_stale = True
        with self.assertRaisesRegex(AssertionError, "accepted"):
            review.assert_stale_rejected()
        self.assertFalse(f.member._closed)
        self.assertTrue(f.member._remove_attempted)
        self.assertEqual(f.member._remove_id, f.posts("remove")[0]["request_id"])
        self.assertEqual(f.member._remove_operation["operation_id"], f.member._remove_id)
        f.finish_resources()
        f.member.close()
        self.assertTrue(f.member._closed)
        self.assertEqual(len(f.posts("remove")), 1)

    def test_review_requires_exact_owned_live_resource_rows_for_every_affected_role(self):
        for kind in ("missing", "unrelated", "closed", "changed_row", "foreign_bed"):
            with self.subTest(kind=kind):
                f = self.fixture()
                resources = list(f.resources)
                if kind == "missing":
                    resources.pop()
                elif kind == "unrelated":
                    other = SimpleNamespace(**vars(resources[0]))
                    other.dut = f.main
                    other._testbed = f.bed
                    other.activity_id = "2" * 32
                    other._created_row = f.editor_row(f.config["activities"][0])
                    f.bed._activities.append(other)
                    resources.append(other)
                elif kind == "closed":
                    resources[0]._closed = True
                elif kind == "changed_row":
                    resources[0]._created_row["name"] = "Different owned row"
                else:
                    resources[0]._testbed = object()
                with self.assertRaises((MembershipConflict, AssertionError, ValueError)):
                    f.member.review_removal(*resources)
                self.assertEqual(f.posts("remove"), [])

    def test_retained_records_or_unrelated_changes_cannot_complete_removal_proof(self):
        corruptions = {
            "owner": lambda f: f.config["octopuses"].append(f.owner(f.candidate.identity.device_id)),
            "equipment": lambda f: f.config["components"].append({"component_id": "e" * 32, "label": "Retained",
                                                                   "qualified_names": [{"octopus_id": f.candidate.identity.device_id}]}),
            "unrelated_activity": lambda f: f.config["activities"].clear(),
            "unrelated_slot": lambda f: f.config["external_slots"][0].update(alias="Damaged shortcut"),
            "affected_slot": lambda f: f.config["external_slots"][1].update(alias="Retained removed name", hidden=True),
            "order": lambda f: f.config["activity_order"].append(f.resources[0].activity_id),
            "zone_order": lambda f: f.config["zones"][0]["activity_order"].append(f.resources[0].activity_id),
            "main_boot": lambda f: f.system.update(boot_id="9" * 32),
            "main_settings": lambda f: setattr(f, "main_settings_revision", "99"),
        }
        for description, corrupt in corruptions.items():
            with self.subTest(description=description):
                f = self.fixture()
                review = f.member.review_removal(*f.resources)
                f.after_remove = corrupt
                with self.assertRaises((MembershipConflict, AssertionError)):
                    review.confirm()
                self.assertFalse(f.member._closed)

    def test_review_cannot_claim_preexisting_or_changed_target_equipment(self):
        for kind in ("unowned_record", "changed_record", "missing_creation", "baseline_owner"):
            with self.subTest(kind=kind):
                f = self.fixture()
                if kind == "unowned_record":
                    record = deepcopy(f.config["components"][-1])
                    record["component_id"] = "e" * 32
                    record["qualified_names"][0]["osd_name"] = "Unowned device"
                    f.config["components"].append(record)
                elif kind == "changed_record":
                    f.config["components"][-1]["label"] = "Concurrent equipment edit"
                elif kind == "missing_creation":
                    f.resources[0]._new_equipment = {}
                else:
                    f.member._baseline["octopuses"].append(f.owner(f.candidate.identity.device_id))
                with self.assertRaises((MembershipConflict, AssertionError)):
                    f.member.review_removal(*f.resources)
                self.assertEqual(f.posts("remove"), [])

    def test_member_name_settings_or_unconfirmed_reset_cannot_pass(self):
        for fault in ("keep_peer", "keep_name", "keep_settings", "unscheduled_reset"):
            with self.subTest(fault=fault):
                f = self.fixture()
                review = f.member.review_removal(*f.resources)
                if fault == "unscheduled_reset":
                    f.reset_mode = "unconfirmed"
                else:
                    setattr(f, fault, True)
                with self.assertRaises((MembershipConflict, AssertionError, TimeoutError)):
                    review.confirm()
                self.assertFalse(f.member._closed)

    def test_expired_member_settings_are_refreshed_without_changing_review_scope(self):
        f = self.fixture()
        path = "/api/devices/" + f.candidate.identity.device_id + "/settings"
        before = sum(route == path for route, _ in f.calls)
        f.settings_reads_pending = 2
        review = f.member.review_removal(*f.resources)
        self.assertEqual(sum(route == path for route, _ in f.calls) - before, 3)
        f.settings_reads_pending = 1
        review.confirm()
        self.assertTrue(f.member._closed)
        self.assertEqual(len(f.posts("remove")), 1)

    def test_reading_settings_cannot_hide_changed_member_boot_or_configuration(self):
        for kind in ("member_boot", "configuration"):
            with self.subTest(kind=kind):
                f = self.fixture()
                f.settings_reads_pending = 1
                if kind == "member_boot":
                    f.reading_effect = lambda fixture: fixture.system["membership"]["peers"][-1].update(boot_id="9" * 32)
                else:
                    f.reading_effect = lambda fixture: fixture.advance()
                with self.assertRaises(MembershipConflict):
                    f.member.review_removal(*f.resources)
                self.assertEqual(f.posts("remove"), [])

    def test_lost_admitted_reply_reconciles_original_operation_without_second_remove(self):
        f = self.fixture()
        review = f.member.review_removal(*f.resources)
        f.lose_remove_reply = True
        try:
            review.confirm()
        except OSError:
            pass
        if not f.member._closed:
            self.assertTrue(f.member._remove_attempted)
            self.assertEqual(f.member._remove_id, f.posts("remove")[0]["request_id"])
            f.finish_resources()
            f.member.close()
        self.assertTrue(f.member._closed)
        self.assertEqual(len(f.posts("remove")), 1)

    def test_unavailable_admitted_result_remains_owned_for_later_cleanup_without_resubmission(self):
        f = self.fixture()
        review = f.member.review_removal(*f.resources)
        f.lose_remove_reply = True
        f.missing_operation_reads = 1
        with self.assertRaises(AmityHTTPError):
            review.confirm()
        self.assertFalse(f.member._closed)
        self.assertTrue(f.member._remove_attempted)
        self.assertEqual(f.member._remove_id, f.posts("remove")[0]["request_id"])
        f.finish_resources()
        f.member.close()
        self.assertTrue(f.member._closed)
        self.assertEqual(len(f.posts("remove")), 1)

    def test_server_error_after_admission_reconciles_original_operation_without_second_remove(self):
        f = self.fixture()
        review = f.member.review_removal(*f.resources)
        f.remove_http_error = 500
        review.confirm()
        self.assertTrue(f.member._closed)
        self.assertEqual(len(f.posts("remove")), 1)
        self.assertEqual(f.member._remove_id, f.posts("remove")[0]["request_id"])

    def test_actual_bed_report_retains_complete_cascade_proofs_and_discards_private_fields(self):
        f = self.fixture()
        stale = f.member.review_removal(*f.resources)
        f.add_unrelated()
        stale.assert_stale_rejected()
        fresh = f.member.review_removal(*f.resources)
        fresh.confirm()
        expected = deepcopy([stale.proof, fresh.proof])
        common = ("kind", "request_id", "device_id", "main_boot", "member_boot",
                  "affected_activity_ids", "affected_activities", "role_activities",
                  "reviewed_config_revision", "reviewed_presentation_revision",
                  "reviewed_membership_revision")
        stale_fields = ("stale_rejected", "http_status", "error_code", "operation_not_admitted",
                        "configuration_unchanged", "membership_unchanged", "settings_unchanged",
                        "unchanged_config_sha256")
        removed_fields = ("removed", "operation_stage", "reset", "exact_configuration",
                          "deleted_activity_ids", "deleted_component_ids", "preserved_activity_ids",
                          "expected_config_sha256", "actual_config_sha256", "inventory_removed",
                          "settings_purged", "unrelated_settings_preserved")
        for proof, extra in zip(expected, (stale_fields, removed_fields)):
            self.assertTrue(set(common + extra) <= set(proof))
            self.assertEqual(proof["device_id"], f.candidate.identity.device_id)
            self.assertEqual(set(proof["affected_activity_ids"]), {r.activity_id for r in f.resources})
            self.assertEqual(set(proof["role_activities"]), set(f.roles))
            self.assertTrue(all(proof["role_activities"][role] for role in f.roles))
        self.assertEqual(expected[0]["http_status"], 409)
        self.assertEqual(expected[0]["error_code"], "stale_removal")
        self.assertEqual(len(expected[0]["unchanged_config_sha256"]), 64)
        self.assertEqual(expected[1]["reset"], "scheduled")
        self.assertEqual(expected[1]["expected_config_sha256"], expected[1]["actual_config_sha256"])
        self.assertTrue(expected[1]["settings_purged"])
        self.assertTrue(expected[1]["unrelated_settings_preserved"])

        log = []
        dut = bed_fixture.UnitDUT("first-board", log)
        bed = TestBed(dut, session_factory=lambda components: bed_fixture.UnitSession(components, log))
        with bed:
            bed.control_observations = [
                {**proof, "private_token": "PRIVATE_TOKEN", "raw_config": {"password": "PRIVATE_CONFIG"},
                 "headers": {"Authorization": "Bearer PRIVATE_BEARER"}}
                for proof in (stale.proof, fresh.proof)]
        serialized = bed.report
        self.assertEqual(serialized["control_observations"], expected)
        self.assertNotIn("PRIVATE_", json.dumps(serialized))
        stale.proof["role_activities"]["source"].clear()
        fresh.proof["affected_activities"][0]["name"] = "Changed after serialization"
        self.assertEqual(serialized["control_observations"], expected)
        serialized["control_observations"][1]["deleted_activity_ids"].clear()
        self.assertEqual(bed.report["control_observations"][1]["deleted_activity_ids"],
                         expected[1]["deleted_activity_ids"])


if __name__ == "__main__":
    unittest.main()
