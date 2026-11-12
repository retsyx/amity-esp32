"""Add/remove exercises existing authenticated UI operations with exact ownership."""

from copy import deepcopy
from pathlib import Path
import json
import sys
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from urllib.error import URLError

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import DUT, TestBed
from amity_harness.dut import AmityHTTPError
from amity_harness.membership import AddedAmity, MembershipCleanupError, MembershipConflict


class FakeGPIO:
    def __init__(self):
        self.calls = []

    def low(self, pin):
        self.calls.append(("low", pin))

    def release(self, pin):
        self.calls.append(("release", pin))


class UnitSession:
    def __init__(self, components=()):
        self.components, self.records = tuple(components), ()
        self.closed = False

    def __enter__(self):
        return self

    def assert_healthy(self):
        pass

    def close(self):
        self.closed = True

    def __exit__(self, *args):
        self.close()


class Fixture:
    token = "PRIVATE_CANDIDATE_TOKEN"
    setup_code = "PRIVATE_SETUP_CODE"

    def __init__(self):
        self.main = DUT("amity-dut")
        self.gpio = FakeGPIO()
        self.candidate = DUT("amity-dut-2", gpio=self.gpio)
        self.main.membership_operations = []
        self.callbacks, self.calls, self.presses, self.operations = [], [], [], {}
        self.group, self.boot, self.member_boot = "a" * 32, "b" * 32, "c" * 32
        self.existing_id, self.wrong_id = "d" * 32, "e" * 32
        self.capacity = len({index for name, index, _ in self.candidate.wiring.cec if name == self.candidate.name})
        self.expected_label = "Amity " + self.candidate.identity.device_id[-6:]
        self.candidates = [{"token": self.token, "label": self.expected_label}]
        self.requires_code = self.lose_add_reply = self.unknown_add = False
        self.bad_add_identity = self.bad_add_boot = self.wrong_candidate = False
        self.foreign_scan = self.reject_add = self.fail_add_operation = False
        self.change_boot_before_remove = False
        self.change_config_before_remove = False
        self.lose_remove_reply = False
        self.adoption_remaining, self.pending_owner = 0, None
        self.reset_mode = "scheduled"
        self.settings_status, self.settings_boot = "ready", self.member_boot
        self.settings_adapter_ids = list(range(self.capacity))
        self.system = {
            "api_version": 2, "mode": "coordinator", "octopus_id": self.main.identity.device_id,
            "consortium_id": self.group, "boot_id": self.boot, "hostname": self.main.name,
            "ready": True, "configuration_pending": False,
            "applied_config_revision": "7", "applied_presentation_revision": "4",
            "named_devices": [{"octopus_id": self.main.identity.device_id, "hostname": self.main.name},
                              {"octopus_id": self.existing_id, "hostname": "existing-member"}],
            "capabilities": {"supported_octopuses": 3},
            "membership": {"membership_revision": "2", "coordinator": True, "authority_ready": True,
                           "member": False, "connected": True, "crypto_ready": True,
                           "enrollment_open": False, "phase": "idle", "last_error": "",
                           "peers": [self.peer(self.existing_id, "existing-member", "f" * 32)]}}
        self.config = {
            "version": 3, "controller_octopus_id": self.main.identity.device_id,
            "consortium_id": self.group, "primary_zone_id": "1" * 32,
            "config_revision": "7", "presentation_revision": "4", "activity_order": ["2" * 32],
            "octopuses": [self.owner(self.main.identity.device_id), self.owner(self.existing_id)],
            "components": [{"component_id": "3" * 32, "label": "Saved equipment", "kind": "source",
                            "qualified_names": [{"consortium_id": self.group,
                                                 "octopus_id": self.main.identity.device_id,
                                                 "tentacle_id": 0, "side": "back", "osd_name": "Saved player"}]}],
            "activities": [{"activity_id": "2" * 32, "zone_id": "1" * 32, "label": "Saved activity",
                            "source": "3" * 32, "display": None, "audio": None,
                            "switch_device": None, "switch_input": 0}],
            "zones": [{"zone_id": "1" * 32, "activity_order": ["2" * 32]}],
            "external_slots": [{"slot": 0, "activity_id": "2" * 32, "tombstone": False,
                                "hidden": False, "alias": "Saved shortcut"}]}
        self.original = deepcopy(self.config)
        self.admission = {"scan_id": None, "phase": "idle", "error": "", "requires_setup_code": False,
                          "candidates": [], "candidate_id": None}
        self.bed = SimpleNamespace(duts=(self.main, self.candidate), control_only=(self.candidate,),
                                   _entered=True, _closed=False, _closing=False, session=UnitSession(),
                                   add_cleanup=self.callbacks.append)
        self.main._testbed = self.candidate._testbed = self.bed
        self.main.request = self.request
        self.main.get_system = lambda: self.request("/api/system")
        self.main.authenticate = lambda: setattr(self.main, "_token", "PRIVATE_LOGIN_TOKEN")
        self.main.logout = lambda: setattr(self.main, "_token", None)
        self.candidate.authenticate = lambda: self.forbidden("Candidate authentication")
        self.candidate.request = lambda *args, **kwargs: self.forbidden("Candidate HTTP")
        original_press = self.candidate.press_fw_reset

        def press(seconds):
            self.presses.append(seconds)
            with patch("amity_harness.dut.time.sleep"):
                original_press(seconds)
            self.admission.update(phase="found", candidates=deepcopy(self.candidates))
            if self.foreign_scan:
                self.admission["scan_id"] = "9" * 32

        self.candidate.press_fw_reset = press

    @staticmethod
    def forbidden(description):
        raise AssertionError(description + " must not occur")

    def owner(self, identity):
        return {"octopus_id": identity, "label": "Saved board", "generated_label": "Saved board",
                "label_override": False, "supported_tentacles": self.capacity}

    def peer(self, identity, hostname, boot):
        return {"octopus_id": identity, "hostname": hostname, "boot_id": boot,
                "enrolled": True, "ready": True, "management_ready": True,
                "supported_tentacles": self.capacity, "configured_tentacles_mask": (1 << self.capacity) - 1,
                "capabilities": 0xffffffff, "firmware_identity": "fixture"}

    def bump(self):
        self.config["config_revision"] = str(int(self.config["config_revision"]) + 1)
        self.system["applied_config_revision"] = self.config["config_revision"]

    def operation(self, request_id, stage, *, kind=None, reset=None):
        result = {"operation_id": "8" * 32 if kind == "add" and self.bad_add_identity else request_id,
                  "stage": stage, "boot_id": "9" * 32 if kind == "add" and self.bad_add_boot else self.boot,
                  "message": "", "retryable": False, "rebooting": False}
        if reset is not None:
            result["reset"] = reset
        return result

    def request(self, path, data=None, **kwargs):
        self.calls.append((path, deepcopy(data)))
        if data is None:
            if path == "/api/system":
                return deepcopy(self.system)
            if path == "/api/config":
                if self.pending_owner is not None:
                    self.adoption_remaining -= 1
                    if not self.adoption_remaining:
                        self.config["octopuses"].append(self.owner(self.pending_owner))
                        self.pending_owner = None
                        self.system["configuration_pending"] = False
                        self.bump()
                return deepcopy(self.config)
            if path == "/api/membership/admission":
                return deepcopy(self.admission)
            if path == "/api/activity-editor/status":
                return {"current_activity": -1, "activity_settling": False, "expected_boot_id": self.boot}
            if path.startswith("/api/operations/"):
                operation = self.operations.get(path.rsplit("/", 1)[1])
                if operation is None:
                    raise AmityHTTPError(404, "Unknown operation")
                return deepcopy(operation)
            if path == "/api/devices/" + self.candidate.identity.device_id + "/settings":
                return {"status": self.settings_status, "error": "" if self.settings_status == "ready" else "offline",
                        "settings": {"boot_id": self.settings_boot, "settings_revision": "1",
                                     "adapters": [{"tentacle_id": index, "name": "HDMI adapter " + str(index + 1),
                                                   "configured": True,
                                                   "default_front_physical_address": "1.0.0.0",
                                                   "observed_front_physical_address": None,
                                                   "effective_front_physical_address": None}
                                                  for index in self.settings_adapter_ids]}}
            raise AssertionError(path)
        request_id = data["request_id"]
        if path == "/api/membership/scan":
            self.admission.update(scan_id=request_id, phase="scanning", error="",
                                  requires_setup_code=self.requires_code, candidates=[], candidate_id=None)
            result = self.operation(request_id, "complete")
        elif path == "/api/membership/cancel":
            if data["scan_id"] != self.admission["scan_id"]:
                raise AmityHTTPError(409, "stale_scan")
            self.admission["phase"] = "cancelled"
            result = self.operation(request_id, "complete")
        elif path == "/api/membership/add":
            if self.reject_add:
                raise AmityHTTPError(409, "Addition rejected")
            if self.unknown_add:
                raise OSError("Add result is unknown")
            if self.fail_add_operation:
                self.admission.update(phase="failed", candidate_id=self.candidate.identity.device_id)
                self.operations[request_id] = self.operation(request_id, "failed", kind="add")
                return self.operation(request_id, "admitted", kind="add")
            identity = self.wrong_id if self.wrong_candidate else self.candidate.identity.device_id
            self.admission.update(phase="complete", candidate_id=identity)
            self.system["membership"]["membership_revision"] = "3"
            self.system["membership"]["peers"].append(self.peer(identity, data["name"], self.member_boot))
            self.system["named_devices"].append({"octopus_id": identity, "hostname": data["name"]})
            if self.adoption_remaining:
                self.pending_owner = identity
                self.system["configuration_pending"] = True
            else:
                if not any(row["octopus_id"] == identity for row in self.config["octopuses"]):
                    self.config["octopuses"].append(self.owner(identity))
                self.bump()
            self.operations[request_id] = self.operation(request_id, "complete", kind="add")
            if self.lose_add_reply:
                raise OSError("Add response was lost after admission")
            return self.operation(request_id, "admitted", kind="add")
        elif path == "/api/membership/remove":
            target = next(p for p in self.system["membership"]["peers"] if p["octopus_id"] == data["octopus_id"])
            if self.change_boot_before_remove:
                target["boot_id"] = "9" * 32
            if self.change_config_before_remove:
                self.bump()
            if (data["expected_target_boot_id"] != target["boot_id"]
                    or data["expected_membership_revision"] != self.system["membership"]["membership_revision"]
                    or data["expected_config_revision"] != self.config["config_revision"]
                    or data["expected_presentation_revision"] != self.config["presentation_revision"]):
                raise AmityHTTPError(409, "Removal review changed")
            owned = {row["component_id"] for row in self.config["components"]
                     if any(name["octopus_id"] == data["octopus_id"] for name in row["qualified_names"])}
            affected = [row["activity_id"] for row in self.config["activities"]
                        if any(row.get(role) in owned for role in ("source", "display", "audio", "switch_device"))]
            if data["affected_activity_ids"] != affected:
                raise AmityHTTPError(409, "Affected activities changed")
            self.system["membership"]["membership_revision"] = str(int(self.system["membership"]["membership_revision"]) + 1)
            self.system["membership"]["peers"] = [p for p in self.system["membership"]["peers"] if p["octopus_id"] != data["octopus_id"]]
            self.config["octopuses"] = [row for row in self.config["octopuses"] if row["octopus_id"] != data["octopus_id"]]
            if "adapter_settings" in self.config:
                self.config["adapter_settings"] = [row for row in self.config["adapter_settings"]
                                                   if row["octopus_id"] != data["octopus_id"]]
            self.config["components"] = [row for row in self.config["components"] if row["component_id"] not in owned]
            self.config["activities"] = [row for row in self.config["activities"] if row["activity_id"] not in affected]
            self.system["named_devices"] = [row for row in self.system["named_devices"] if row["octopus_id"] != data["octopus_id"]]
            self.bump()
            self.operations[request_id] = self.operation(request_id, "complete", reset=self.reset_mode)
            if self.lose_remove_reply:
                self.lose_remove_reply = False
                raise OSError("Removal response was lost after reset was scheduled")
            return self.operation(request_id, "admitted")
        else:
            raise AssertionError(path)
        self.operations[request_id] = result
        return deepcopy(result)

    def added(self, *, name=None, timeout=0.1, setup_code=None):
        return AddedAmity(self.main, self.candidate, name=name, timeout=timeout, setup_code=setup_code)

    def posts(self, suffix):
        return [data for path, data in self.calls if path == "/api/membership/" + suffix and data is not None]


class MembershipTests(unittest.TestCase):
    def setUp(self):
        # Polling is driven by deterministic fixture replies; no real radio,
        # GPIO hold or UI polling delay belongs in these unit checks.
        sleeper = patch("amity_harness.membership.time.sleep")
        sleeper.start()
        self.addCleanup(sleeper.stop)

    def test_earlier_member_checks_accept_a_later_proven_member_with_its_own_fences(self):
        f = Fixture()
        member = f.added(timeout=0.2).create()
        member.assert_connected()
        later_id = "9" * 32
        later = SimpleNamespace(name="amity-later", member_boot="8" * 32, _closed=False)
        f.system["membership"]["peers"].append(f.peer(later_id, later.name, later.member_boot))
        system = deepcopy(f.system)
        with self.assertRaisesRegex(MembershipConflict, "changed unexpectedly"):
            member._preserved_members(system, added=True)
        f.main._testbed._member_proofs[later_id] = later
        self.assertIn(later_id, member._preserved_members(system, added=True))
        for change in ({"boot_id": "7" * 32}, {"hostname": "renamed"}, {"enrolled": False, "provisional": True}):
            with self.subTest(change=change):
                changed = deepcopy(system)
                next(peer for peer in changed["membership"]["peers"] if peer["octopus_id"] == later_id).update(change)
                with self.assertRaisesRegex(MembershipConflict, "later added Amity changed"):
                    member._preserved_members(changed, added=True)

    def test_add_verifies_existing_ui_evidence_and_removal_resets_only_the_owned_member(self):
        f = Fixture()
        with f.main.add_amity(f.candidate, name="amity-dut2", timeout=0.2) as member:
            self.assertIs(member.assert_connected(), member)
            self.assertEqual(len(f.posts("scan")), 1)
            self.assertEqual(len(f.posts("add")), 1)
            self.assertEqual(f.posts("add")[0]["name"], "amity-dut2")
            self.assertEqual(f.presses, [3.5])
            pin = f.candidate.wiring.reset_gpio(f.candidate.name)
            self.assertEqual(f.gpio.calls, [("low", pin), ("release", pin)])
        removal = f.posts("remove")
        self.assertEqual(len(removal), 1)
        self.assertEqual(set(removal[0]), {"request_id", "octopus_id", "confirm", "expected_membership_revision", "expected_target_boot_id",
                                          "expected_config_revision", "expected_presentation_revision", "affected_activity_ids"})
        self.assertEqual(removal[0]["octopus_id"], f.candidate.identity.device_id)
        self.assertEqual(removal[0]["expected_membership_revision"], "3")
        self.assertEqual(removal[0]["expected_target_boot_id"], f.member_boot)
        self.assertIs(removal[0]["confirm"], True)
        self.assertEqual(removal[0]["affected_activity_ids"], [])
        self.assertFalse(any(row["octopus_id"] == f.candidate.identity.device_id for row in f.config["octopuses"]))
        self.assertEqual([p["octopus_id"] for p in f.system["membership"]["peers"]], [f.existing_id])
        for field in ("activities", "components", "external_slots", "zones", "controller_octopus_id"):
            self.assertEqual(f.config[field], f.original[field])
        member.close()
        self.assertEqual(len(f.posts("remove")), 1)

    def test_default_name_and_repeated_create_enter_do_not_duplicate_addition(self):
        f = Fixture()
        member = f.added()
        self.assertIs(member.create(), member)
        self.assertIs(member.create(), member)
        with member:
            member.assert_connected()
        self.assertEqual(f.posts("add")[0]["name"], f.candidate.name)
        self.assertEqual(len(f.posts("add")), 1)
        self.assertEqual(len(f.posts("remove")), 1)

    def test_cleanup_purges_retained_candidate_inventory_settings_and_unreferenced_equipment(self):
        f = Fixture()
        target = f.candidate.identity.device_id
        target_component = {"component_id": "5" * 32, "label": "Old candidate equipment", "kind": "source",
                            "qualified_names": [{"consortium_id": f.group, "octopus_id": target,
                                                 "tentacle_id": 0, "side": "back", "osd_name": "Old player"}]}
        f.config["octopuses"].append(f.owner(target))
        f.config["components"].append(target_component)
        f.config["adapter_settings"] = [{"octopus_id": f.main.identity.device_id, "tentacle_id": 0},
                                        {"octopus_id": target, "tentacle_id": 0}]
        before = deepcopy(f.config)
        with f.added().create():
            pass
        self.assertEqual(f.config["octopuses"], before["octopuses"][:-1])
        self.assertEqual(f.config["components"], before["components"][:-1])
        self.assertEqual(f.config["adapter_settings"], before["adapter_settings"][:1])
        for key in set(before) - {"octopuses", "components", "adapter_settings", "config_revision", "presentation_revision"}:
            self.assertEqual(f.config[key], before[key], key)
        removal = f.posts("remove")[0]
        self.assertEqual(removal["affected_activity_ids"], [])
        self.assertEqual(removal["expected_config_revision"], str(int(before["config_revision"]) + 1))
        self.assertEqual(removal["expected_presentation_revision"], before["presentation_revision"])
        self.assertFalse(any(row["octopus_id"] == target for row in f.system["named_devices"]))

    def test_candidate_referenced_by_existing_activity_is_rejected_before_scan_or_gpio(self):
        for role in ("display", "source", "audio", "switch_device"):
            with self.subTest(role=role):
                f = Fixture()
                target = f.candidate.identity.device_id
                f.config["components"].append({"component_id": "5" * 32, "label": "Saved candidate source", "kind": "source",
                                                "qualified_names": [{"consortium_id": f.group, "octopus_id": target,
                                                                     "tentacle_id": 0, "side": "back", "osd_name": "Saved source"}]})
                f.config["activities"][0][role] = "5" * 32
                before = deepcopy(f.config)
                member = f.added()
                with self.assertRaises(MembershipConflict):
                    member.create()
                member.close()
                self.assertEqual(f.posts("scan"), [])
                self.assertEqual(f.posts("add"), [])
                self.assertEqual(f.posts("remove"), [])
                self.assertEqual(f.gpio.calls, [])
                self.assertEqual(f.config, before)

    def test_cleanup_never_deletes_a_concurrent_user_activity_selection(self):
        f = Fixture()
        member = f.added().create()
        f.config["components"].append({"component_id": "5" * 32, "label": "User equipment", "kind": "source",
                                        "qualified_names": [{"consortium_id": f.group, "octopus_id": f.candidate.identity.device_id,
                                                             "tentacle_id": 0, "side": "back", "osd_name": "User source"}]})
        f.config["activities"][0]["source"] = "5" * 32
        before = deepcopy(f.config)
        with self.assertRaises(MembershipCleanupError):
            member.close()
        self.assertEqual(f.posts("remove"), [])
        self.assertEqual(f.config, before)

    def test_cleanup_rejects_retained_owner_settings_or_equipment_after_reset_acknowledgment(self):
        for key in ("octopuses", "adapter_settings", "components"):
            with self.subTest(key=key):
                f = Fixture()
                target = f.candidate.identity.device_id
                rows = {"octopuses": f.owner(target), "adapter_settings": {"octopus_id": target, "tentacle_id": 0},
                        "components": {"component_id": "5" * 32, "label": "Old source", "kind": "source",
                                       "qualified_names": [{"consortium_id": f.group, "octopus_id": target,
                                                            "tentacle_id": 0, "side": "back", "osd_name": "Old source"}]}}
                f.config.setdefault(key, []).append(deepcopy(rows[key]))
                member = f.added().create()
                original_request = f.request

                def request(path, data=None, **kwargs):
                    response = original_request(path, data, **kwargs)
                    if path == "/api/membership/remove" and data is not None:
                        f.config[key].append(deepcopy(rows[key]))
                    return response

                f.request = f.main.request = request
                with self.assertRaises(MembershipCleanupError):
                    member.close()
                self.assertEqual(len(f.posts("remove")), 1)
                self.assertFalse(any(peer["octopus_id"] == target for peer in f.system["membership"]["peers"]))

    def test_nested_cleanup_accepts_only_a_completed_test_owned_inventory_removal(self):
        for completed in (True, False):
            with self.subTest(completed=completed):
                f = Fixture()
                third = DUT("amity-dut-3")
                third._testbed = f.bed
                f.bed.control_only = ()
                f.bed.members = (f.candidate, third)
                f.config["octopuses"].append(f.owner(third.identity.device_id))
                member = f.added().create()
                f.config["octopuses"] = [row for row in f.config["octopuses"] if row["octopus_id"] != third.identity.device_id]
                if completed:
                    f.main.membership_operations.append({"phase": "cleanup", "device_id": third.identity.device_id,
                                                         "operation_stage": "complete", "reset": "scheduled",
                                                         "retained_member": False})
                    member.close()
                    self.assertEqual(len(f.posts("remove")), 1)
                else:
                    with self.assertRaises(MembershipCleanupError):
                        member.close()
                    self.assertEqual(f.posts("remove"), [])

    def test_cleanup_rejects_retained_named_device_inventory_after_reset_acknowledgment(self):
        f = Fixture()
        member = f.added().create()
        original_request = f.request

        def request(path, data=None, **kwargs):
            response = original_request(path, data, **kwargs)
            if path == "/api/membership/remove" and data is not None:
                f.system["named_devices"].append({"octopus_id": f.candidate.identity.device_id,
                                                 "hostname": f.candidate.name})
            return response

        f.request = f.main.request = request
        with self.assertRaises(MembershipCleanupError):
            member.close()
        self.assertEqual(len(f.posts("remove")), 1)
        self.assertEqual(f.main.membership_operations[-1]["reset"], "scheduled")

    def test_prior_bed_removal_receipt_does_not_authorize_inventory_changes(self):
        f = Fixture()
        third = DUT("amity-dut-3")
        third._testbed = f.bed
        f.bed.members = (third,)
        f.config["octopuses"].append(f.owner(third.identity.device_id))
        f.main.membership_operations.append({"phase": "cleanup", "device_id": third.identity.device_id,
                                             "operation_stage": "complete", "reset": "scheduled", "retained_member": False})
        f.bed._initial_membership_counts = {id(f.main): len(f.main.membership_operations)}
        member = f.added().create()
        f.config["octopuses"] = [row for row in f.config["octopuses"] if row["octopus_id"] != third.identity.device_id]
        with self.assertRaises(MembershipCleanupError):
            member.close()
        self.assertEqual(f.posts("remove"), [])

    def test_add_waits_for_async_owner_adoption_and_applied_configuration(self):
        f = Fixture()
        f.adoption_remaining = 3
        with f.added().create() as member:
            self.assertIsNone(f.pending_owner)
            self.assertEqual(f.adoption_remaining, 0)
            self.assertFalse(f.system["configuration_pending"])
            self.assertEqual(f.system["applied_config_revision"], f.config["config_revision"])
            self.assertEqual(len([row for row in f.config["octopuses"]
                                  if row["octopus_id"] == f.candidate.identity.device_id]), 1)
            member.assert_connected()
        self.assertEqual(len(f.posts("add")), 1)

    def test_pending_discovery_polls_only_admission_then_confirms_main_before_add(self):
        f = Fixture()
        original_request = f.request
        remaining = 3

        def request(path, data=None, **kwargs):
            nonlocal remaining
            response = original_request(path, data, **kwargs)
            if path == "/api/membership/admission" and data is None and f.presses and not f.posts("add") and remaining:
                remaining -= 1
                response.update(phase="scanning", candidates=[])
            return response

        f.request = f.main.request = request
        with f.added(timeout=1).create():
            self.assertEqual(remaining, 0)
            scan_index = next(index for index, (path, data) in enumerate(f.calls)
                              if path == "/api/membership/scan" and data is not None)
            add_index = next(index for index, (path, data) in enumerate(f.calls)
                             if path == "/api/membership/add" and data is not None)
            between = f.calls[scan_index + 1:add_index]
            self.assertEqual([path for path, _ in between],
                             ["/api/membership/admission"] * 4 + ["/api/system"])
            trace = next(row for row in f.main.membership_operations if row["phase"] == "add")
            requests = trace["requests"]
            started = next(index for index, row in enumerate(requests) if row["path"] == "/api/membership/scan")
            submitted = next(index for index, row in enumerate(requests) if row["path"] == "/api/membership/add")
            polls = requests[started + 1:submitted]
            self.assertEqual([row.get("admission_phase") for row in polls[:-1]], ["scanning"] * 3 + ["found"])
            self.assertEqual(polls[-1]["path"], "/api/system")
            self.assertEqual(trace["reset_gpio"], f.candidate.wiring.reset_gpio(f.candidate.name))
            self.assertEqual(trace["hold_seconds"], 3.5)
            self.assertIs(trace["released"], True)

    def test_failed_read_is_traced_without_retry_and_nested_reports_exclude_secrets(self):
        f = Fixture()
        f.main._testbed = f.candidate._testbed = None
        original_request = f.request
        failed = False

        def request(path, data=None, **kwargs):
            nonlocal failed
            if path == "/api/membership/admission" and data is None and f.presses and not failed:
                failed = True
                f.calls.append((path, None))
                raise URLError(TimeoutError("synthetic TLS handshake timeout"))
            return original_request(path, data, **kwargs)

        f.request = f.main.request = request
        with TestBed(f.main, f.candidate, control_only=(f.candidate,), session_factory=UnitSession) as bed:
            member = f.added()
            with self.assertRaises(URLError):
                member.create()
            trace = next(row for row in f.main.membership_operations if row["phase"] == "add")
            self.assertEqual(trace["step"], "discover_candidate")
            self.assertEqual(trace["error_type"], "URLError")
            failed_request = trace["requests"][-1]
            self.assertEqual((failed_request["method"], failed_request["path"], failed_request["error_type"]),
                             ("GET", "/api/membership/admission", "URLError"))
            self.assertGreaterEqual(failed_request["finished_ns"], failed_request["started_ns"])
            self.assertGreaterEqual(failed_request["duration_ms"], 0)
            self.assertEqual(len([row for row in trace["requests"]
                                  if row["path"] == "/api/membership/admission"]), 2)  # Preflight plus one failed read.
            self.assertEqual(f.posts("add"), [])
            member.close()
            self.assertEqual(len(f.posts("cancel")), 1)
            self.assertEqual(f.posts("remove"), [])
            # Report allowlists must apply recursively even if a future caller
            # adds raw diagnostics to an otherwise legitimate trace record.
            failed_request.update(body={"setup_code": f.setup_code}, response={"candidate_token": f.token},
                                  Authorization="PRIVATE_LOGIN_TOKEN")
            trace["candidate_token"] = f.token
        report = bed.report
        self.assertNotIn(f.setup_code, json.dumps(report))
        self.assertNotIn(f.token, json.dumps(report))
        self.assertNotIn("PRIVATE_LOGIN_TOKEN", json.dumps(report))
        recorded = next(row for row in report["memberships"] if row["phase"] == "add")["requests"][-1]
        self.assertEqual(recorded["error_type"], "URLError")
        self.assertIn("duration_ms", recorded)
        self.assertFalse({"body", "response", "Authorization"} & set(recorded))
        recorded["duration_ms"] = -1
        fresh = next(row for row in bed.report["memberships"] if row["phase"] == "add")["requests"][-1]
        self.assertGreaterEqual(fresh["duration_ms"], 0)

    def test_lost_remove_response_reconciles_original_reset_receipt_without_another_post(self):
        f = Fixture()
        member = f.added().create()
        f.lose_remove_reply = True
        with self.assertRaises(MembershipCleanupError):
            member.close()
        original_id = f.posts("remove")[0]["request_id"]
        self.assertFalse(any(p["octopus_id"] == f.candidate.identity.device_id
                             for p in f.system["membership"]["peers"]))
        member.close()
        self.assertEqual(len(f.posts("remove")), 1)
        self.assertTrue(any(path == "/api/operations/" + original_id for path, _ in f.calls))
        self.assertEqual(f.main.membership_operations[-1]["reset"], "scheduled")
        self.assertFalse(f.main.membership_operations[-1]["retained_member"])

    def test_rejected_remove_review_gets_a_fresh_identity_and_current_revisions(self):
        f = Fixture()
        member = f.added().create()
        original_request = f.request
        first = True

        def request(path, data=None, **kwargs):
            nonlocal first
            if path == "/api/membership/remove" and data is not None and first:
                first = False
                f.change_config_before_remove = True
                try:
                    return original_request(path, data, **kwargs)
                finally:
                    f.change_config_before_remove = False
            return original_request(path, data, **kwargs)

        f.main.request = f.request = request
        member.close()
        first, fresh = f.posts("remove")
        self.assertNotEqual(first["request_id"], fresh["request_id"])
        self.assertEqual(int(fresh["expected_config_revision"]), int(first["expected_config_revision"]) + 1)
        self.assertEqual(first["expected_target_boot_id"], fresh["expected_target_boot_id"])
        self.assertEqual(first["affected_activity_ids"], fresh["affected_activity_ids"])
        self.assertEqual(f.main.membership_operations[-1]["reset"], "scheduled")

    def test_rejected_review_does_not_authorize_removing_concurrent_user_changes(self):
        f = Fixture()
        member = f.added().create()
        original_request = f.request

        def request(path, data=None, **kwargs):
            if path == "/api/membership/remove" and data is not None:
                f.calls.append((path, deepcopy(data)))
                f.config["activities"][0]["label"] = "User changed this activity"
                f.bump()
                raise AmityHTTPError(409, "The reviewed configuration changed")
            return original_request(path, data, **kwargs)

        f.main.request = f.request = request
        with self.assertRaises(MembershipCleanupError):
            member.close()
        self.assertEqual(len(f.posts("remove")), 1)
        self.assertEqual(f.config["activities"][0]["label"], "User changed this activity")
        self.assertTrue(any(row["octopus_id"] == member.device_id for row in f.system["membership"]["peers"]))

    def test_http_cleanup_can_reset_the_known_member_after_the_cec_worker_failed(self):
        f = Fixture()
        member = f.added().create()

        def failed_worker():
            raise RuntimeError("CEC responder failed")

        f.bed.session.assert_healthy = failed_worker
        member.close()
        self.assertEqual(len(f.posts("remove")), 1)
        self.assertEqual(f.main.membership_operations[-1]["reset"], "scheduled")

    def test_member_context_waits_for_registered_activity_cleanup_before_removal(self):
        f = Fixture()
        member = f.added().create()
        pending = SimpleNamespace(dut=f.main, _closed=False, _write_attempted=True, _creation_rejected=False)
        f.bed._activities = [pending]
        member.close()
        self.assertTrue(member._cleanup_deferred)
        self.assertFalse(member._closed)
        self.assertEqual(f.posts("remove"), [])
        pending._closed = True
        member.close()
        self.assertTrue(member._closed)
        self.assertEqual(f.main.membership_operations[-1]["reset"], "scheduled")

    def test_member_context_cannot_overtake_a_pending_settings_restore(self):
        f = Fixture()
        member = f.added().create()

        class SettingsRestore:
            _cleanup_priority = 1

            def __init__(self):
                self.main, self._closed = f.main, False

            def close(self):
                self._closed = True

        setting = SettingsRestore()
        f.bed._cleanups = [setting.close]
        member.close()
        self.assertEqual(f.posts("remove"), [])
        self.assertTrue(member._cleanup_deferred)
        setting.close()
        member.close()
        self.assertEqual(f.main.membership_operations[-1]["reset"], "scheduled")

    def test_later_member_cleanup_accepts_an_earlier_owned_removal_receipt(self):
        for reset, verified in (("scheduled", False), ("unconfirmed", True)):
            with self.subTest(reset=reset, verified=verified):
                f = Fixture()
                f.system["capabilities"]["supported_octopuses"] = 4
                third = DUT("amity-dut-3")
                third._testbed = f.bed
                f.bed.members = (f.candidate, third)
                f.system["membership"]["peers"].append(f.peer(third.identity.device_id, third.name, "7" * 32))
                f.config["octopuses"].append(f.owner(third.identity.device_id))
                member = f.added().create()
                f.system["membership"]["peers"] = [row for row in f.system["membership"]["peers"]
                                                    if row["octopus_id"] != third.identity.device_id]
                f.config["octopuses"] = [row for row in f.config["octopuses"] if row["octopus_id"] != third.identity.device_id]
                f.main.membership_operations.append({"phase": "cleanup", "device_id": third.identity.device_id,
                                                     "operation_stage": "complete", "reset": reset, "retained_member": False,
                                                     "inventory_removed": verified,
                                                     **({"error_type": "MembershipConflict"} if reset != "scheduled" else {})})
                member.close()
                self.assertEqual(f.main.membership_operations[-1]["reset"], "scheduled")

    def test_real_testbed_skips_candidate_http_and_cleans_member_before_main_logout(self):
        f = Fixture()
        f.main._testbed = f.candidate._testbed = None
        with TestBed(f.main, f.candidate, control_only=(f.candidate,), session_factory=UnitSession) as bed:
            f.main.add_amity(f.candidate, timeout=0.2).assert_connected()
            self.assertEqual(bed.control_only, (f.candidate,))
        self.assertEqual(len(f.posts("remove")), 1)
        self.assertIsNone(f.main._token)
        self.assertIsNone(f.candidate._token)
        self.assertIsNone(f.candidate._testbed)
        self.assertTrue(bed.report["passed"])
        report = json.dumps(bed.report)
        self.assertNotIn(f.token, report)
        self.assertNotIn("PRIVATE_LOGIN_TOKEN", report)

    def test_candidate_must_share_an_active_testbed_and_be_explicitly_control_only(self):
        for change in ("not_control_only", "foreign_bed", "closed_bed"):
            with self.subTest(change=change):
                f = Fixture()
                if change == "not_control_only":
                    f.bed.control_only = ()
                elif change == "foreign_bed":
                    f.candidate._testbed = object()
                else:
                    f.bed._closed = True
                with self.assertRaises((ValueError, RuntimeError)):
                    f.added().create()
                self.assertEqual(f.calls, [])
                self.assertEqual(f.gpio.calls, [])

    def test_headless_component_member_registers_only_verified_boot_and_loses_proof_on_removal(self):
        f = Fixture()
        f.bed.control_only = ()
        f.bed.members = (f.candidate,)
        member = f.added().create()
        self.assertIs(f.bed._member_proofs[f.candidate.identity.device_id], member)
        self.assertEqual(member.member_boot, f.member_boot)
        member.close()
        self.assertEqual(f.bed._member_proofs, {})
        self.assertIsNone(f.candidate._token)

    def test_membership_cleanup_accepts_only_exact_retired_completed_fixture_slots(self):
        f = Fixture()
        member = f.added().create()
        activity_id = "7" * 32
        f.main.activity_operations.extend([
            {"phase": "create", "activity_id": activity_id, "operation": {"stage": "applied"}},
            {"phase": "cleanup", "activity_id": activity_id, "removed_activity": True, "retained_equipment": []}])
        f.config["external_slots"].append({"slot": 1, "activity_id": activity_id,
                                            "tombstone": True, "hidden": False, "alias": ""})
        member.close()
        self.assertEqual(f.posts("remove")[0]["octopus_id"], f.candidate.identity.device_id)

    def test_retired_fixture_slot_exception_rejects_foreign_edits_and_incomplete_cleanup(self):
        for change in ("foreign_id", "not_retired", "alias", "position", "old_slot", "not_removed", "retained_equipment"):
            with self.subTest(change=change):
                f = Fixture()
                member = f.added().create()
                activity_id = "7" * 32
                f.main.activity_operations.extend([
                    {"phase": "create", "activity_id": activity_id, "operation": {"stage": "applied"}},
                    {"phase": "cleanup", "activity_id": activity_id, "removed_activity": True, "retained_equipment": []}])
                slot = {"slot": 1, "activity_id": activity_id, "tombstone": True, "hidden": False, "alias": ""}
                f.config["external_slots"].append(slot)
                if change == "foreign_id": slot["activity_id"] = "8" * 32
                elif change == "not_retired": slot["tombstone"] = False
                elif change == "alias": slot["alias"] = "User edited"
                elif change == "position": slot["slot"] = 4
                elif change == "old_slot": f.config["external_slots"][0]["alias"] = "User edited"
                elif change == "not_removed": f.main.activity_operations[-1]["removed_activity"] = False
                else: f.main.activity_operations[-1]["retained_equipment"] = ["9" * 32]
                with self.assertRaises(MembershipConflict):
                    member._config(deadline=time.monotonic() + 1)

    def test_wrong_main_role_or_existing_target_is_rejected_before_gpio_or_mutations(self):
        cases = ("member", "no_authority", "wrong_main_id", "preexisting_target", "preexisting_name")
        for change in cases:
            with self.subTest(change=change):
                f = Fixture()
                if change == "member":
                    f.system["membership"]["member"] = True
                elif change == "no_authority":
                    f.system["membership"]["authority_ready"] = False
                elif change == "wrong_main_id":
                    f.system["octopus_id"] = f.wrong_id
                elif change == "preexisting_target":
                    f.system["membership"]["peers"].append(f.peer(f.candidate.identity.device_id, f.candidate.name, f.member_boot))
                else:
                    f.system["membership"]["peers"][0]["hostname"] = f.candidate.name
                    f.system["named_devices"][1]["hostname"] = f.candidate.name
                with self.assertRaises((AssertionError, MembershipConflict)):
                    f.added().create()
                self.assertEqual(f.gpio.calls, [])
                self.assertEqual(f.posts("scan"), [])
                self.assertEqual(f.posts("add"), [])
                self.assertEqual(f.posts("remove"), [])

    def test_missing_or_ambiguous_public_label_never_submits_add_and_cancels_only_own_scan(self):
        for candidates in ([], [{"token": "other", "label": "Amity not-target"}],
                           [{"token": "first", "label": "expected"}, {"token": "second", "label": "expected"}]):
            with self.subTest(candidates=candidates):
                f = Fixture()
                f.candidates = [{**row, "label": f.expected_label if row["label"] == "expected" else row["label"]}
                                for row in candidates]
                member = f.added(timeout=0.02)
                with self.assertRaises((AssertionError, MembershipConflict, TimeoutError)):
                    member.create()
                member.close()
                self.assertEqual(f.posts("add"), [])
                self.assertEqual(f.posts("remove"), [])
                self.assertEqual(len(f.posts("cancel")), 1)
                self.assertEqual(f.posts("cancel")[0]["scan_id"], f.posts("scan")[0]["request_id"])

    def test_foreign_scan_is_rejected_without_cancelling_someone_elses_search(self):
        f = Fixture()
        f.foreign_scan = True
        member = f.added()
        with self.assertRaises((AssertionError, MembershipConflict)):
            member.create()
        try:
            member.close()
        except MembershipCleanupError:
            pass
        self.assertEqual(f.posts("add"), [])
        self.assertEqual(f.posts("remove"), [])
        self.assertFalse(any(p["scan_id"] == "9" * 32 for p in f.posts("cancel")))

    def test_strict_setup_code_is_required_only_for_strict_ui_and_is_never_reported(self):
        f = Fixture()
        f.requires_code = True
        with f.added(setup_code=f.setup_code).create() as member:
            self.assertEqual(f.posts("add")[0]["setup_code"], f.setup_code)
            member.assert_connected()
        serialized = json.dumps(f.main.membership_operations)
        self.assertNotIn(f.setup_code, serialized)
        self.assertNotIn(f.token, serialized)
        normal = Fixture()
        resource = normal.added(setup_code=normal.setup_code)
        with resource.create():
            self.assertNotIn("setup_code", normal.posts("add")[0])
        self.assertNotIn(normal.setup_code, json.dumps(normal.main.membership_operations))
        strict = Fixture()
        strict.requires_code = True
        resource = strict.added()
        with self.assertRaises((ValueError, AssertionError, MembershipConflict)):
            resource.create()
        resource.close()
        self.assertEqual(strict.posts("add"), [])

    def test_wrong_full_candidate_identity_is_not_removed_as_test_owned(self):
        f = Fixture()
        f.wrong_candidate = True
        member = f.added()
        with self.assertRaises((AssertionError, MembershipConflict)):
            member.create()
        with self.assertRaises((AssertionError, MembershipConflict, MembershipCleanupError)):
            member.close()
        self.assertEqual(len(f.posts("add")), 1)
        self.assertEqual(f.posts("remove"), [])
        self.assertTrue(any(p["octopus_id"] == f.wrong_id for p in f.system["membership"]["peers"]))

    def test_add_operation_identity_and_boot_must_match_original_request_and_main(self):
        for field in ("bad_add_identity", "bad_add_boot"):
            with self.subTest(field=field):
                f = Fixture()
                setattr(f, field, True)
                with self.assertRaises((AssertionError, MembershipConflict)):
                    f.added().create()
                self.assertEqual(len(f.posts("add")), 1)

    def test_lost_add_response_reconciles_one_request_instead_of_resubmitting(self):
        f = Fixture()
        f.lose_add_reply = True
        member = f.added()
        try:
            member.create()
        except (OSError, TimeoutError, MembershipConflict):
            pass
        self.assertEqual(len(f.posts("add")), 1)
        original_id = f.posts("add")[0]["request_id"]
        member.close()
        self.assertEqual(len(f.posts("add")), 1)
        self.assertTrue(any(path == "/api/operations/" + original_id for path, _ in f.calls))
        self.assertEqual(len(f.posts("remove")), 1)

    def test_unknown_add_result_never_retries_or_removes_an_unproven_member(self):
        f = Fixture()
        f.unknown_add = True
        member = f.added(timeout=0.02)
        with self.assertRaises((OSError, TimeoutError, MembershipConflict, AssertionError, AmityHTTPError)):
            member.create()
        try:
            member.close()
        except MembershipCleanupError:
            pass
        self.assertEqual(len(f.posts("add")), 1)
        self.assertEqual(f.posts("remove"), [])

    def test_terminal_failed_or_explicitly_rejected_add_without_member_can_cleanup(self):
        for failure in ("fail_add_operation", "reject_add"):
            with self.subTest(failure=failure):
                f = Fixture()
                setattr(f, failure, True)
                member = f.added()
                with self.assertRaises((MembershipConflict, AmityHTTPError)):
                    member.create()
                member.close()
                self.assertEqual(len(f.posts("add")), 1)
                self.assertEqual(f.posts("remove"), [])
                self.assertEqual([p["octopus_id"] for p in f.system["membership"]["peers"]], [f.existing_id])
                self.assertFalse(f.main.membership_operations[-1].get("retained_member"))

    def test_connected_assertion_rejects_reboot_role_or_membership_changes(self):
        for change in ("reboot", "role", "member_boot", "unready", "duplicate", "existing_member", "saved_activity"):
            with self.subTest(change=change):
                f = Fixture()
                member = f.added().create()
                target = next(p for p in f.system["membership"]["peers"] if p["octopus_id"] == f.candidate.identity.device_id)
                if change == "reboot":
                    f.system["boot_id"] = "9" * 32
                elif change == "role":
                    f.system["membership"]["coordinator"] = False
                elif change == "member_boot":
                    target["boot_id"] = "9" * 32
                elif change == "unready":
                    target["ready"] = False
                elif change == "duplicate":
                    f.system["membership"]["peers"].append(deepcopy(target))
                elif change == "existing_member":
                    f.system["membership"]["peers"] = [target]
                else:
                    f.config["activities"][0]["label"] = "Unexpected edit"
                with self.assertRaises((AssertionError, MembershipConflict, TimeoutError)):
                    member.assert_connected()

    def test_connected_settings_poll_survives_slow_tls_without_expiring_the_two_second_cache(self):
        f = Fixture()
        member = f.added().create()
        f.calls.clear()
        original_request = f.request
        clock, cache_received, polls = [0.0], [None], []
        settings_path = "/api/devices/" + f.candidate.identity.device_id + "/settings"

        def request(path, data=None, **kwargs):
            # Each TLS connection takes a second. A shipping settings GET
            # queues an asynchronous reply, then exposes it for two seconds.
            clock[0] += 1
            response = original_request(path, data, **kwargs)
            if path == settings_path:
                polls.append(clock[0])
                if cache_received[0] is None or clock[0] - cache_received[0] >= 2:
                    cache_received[0] = clock[0] + 0.1
                    response.update(status="reading", error="")
            return response

        f.request = f.main.request = request
        with patch("amity_harness.membership.time.monotonic", side_effect=lambda: clock[0]), \
                patch("amity_harness.membership.time.monotonic_ns", side_effect=lambda: int(clock[0] * 1e9)), \
                patch("amity_harness.membership.time.sleep", side_effect=lambda seconds: clock.__setitem__(0, clock[0] + seconds)):
            self.assertIs(member.assert_connected(timeout=20), member)
        # The original system/config/settings loop always missed the reply's
        # freshness window; direct settings polling reads it and then fences
        # the main, full configuration and member identity again.
        self.assertEqual([path for path, _ in f.calls],
                         ["/api/system", "/api/config", settings_path, settings_path,
                          "/api/system", "/api/config"])
        self.assertEqual(len(polls), 2)
        self.assertLess(polls[1] - polls[0], 2)
        self.assertEqual(member.member_boot, f.member_boot)

    def test_connected_settings_success_is_rechecked_against_current_boot_authority_and_configuration(self):
        for change in ("main_boot", "authority", "member_boot", "saved_activity", "membership"):
            with self.subTest(change=change):
                f = Fixture()
                member = f.added().create()
                original_request = f.request

                def request(path, data=None, **kwargs):
                    response = original_request(path, data, **kwargs)
                    if path.endswith("/settings"):
                        target = next(p for p in f.system["membership"]["peers"]
                                      if p["octopus_id"] == f.candidate.identity.device_id)
                        if change == "main_boot":
                            f.system["boot_id"] = "9" * 32
                        elif change == "authority":
                            f.system["membership"]["authority_ready"] = False
                        elif change == "member_boot":
                            target["boot_id"] = "9" * 32
                        elif change == "saved_activity":
                            f.config["activities"][0]["label"] = "Concurrent edit"
                        else:
                            f.system["membership"]["peers"] = [target]
                    return response

                f.request = f.main.request = request
                with self.assertRaises(MembershipConflict):
                    member.assert_connected()

    def test_connected_assertion_requires_exact_names_configuration_and_live_settings(self):
        for change in ("hostname", "named_devices", "owner", "capacity", "owner_capacity",
                       "settings_offline", "settings_boot", "settings_boolean_id", "settings_duplicate_ids"):
            with self.subTest(change=change):
                f = Fixture()
                member = f.added(timeout=0.02).create()
                target = next(p for p in f.system["membership"]["peers"] if p["octopus_id"] == f.candidate.identity.device_id)
                if change == "hostname":
                    target["hostname"] = "another-name"
                elif change == "named_devices":
                    f.system["named_devices"] = [p for p in f.system["named_devices"] if p["octopus_id"] != target["octopus_id"]]
                elif change == "owner":
                    f.config["octopuses"] = [p for p in f.config["octopuses"] if p["octopus_id"] != target["octopus_id"]]
                elif change == "capacity":
                    target["supported_tentacles"] = True
                elif change == "owner_capacity":
                    next(p for p in f.config["octopuses"] if p["octopus_id"] == target["octopus_id"])["supported_tentacles"] = True
                elif change == "settings_offline":
                    f.settings_status = "offline"
                elif change == "settings_boot":
                    f.settings_boot = "9" * 32
                elif change == "settings_boolean_id":
                    f.settings_adapter_ids = [False]
                else:
                    f.settings_adapter_ids = [0, 0]
                with self.assertRaises((AssertionError, MembershipConflict, TimeoutError)):
                    member.assert_connected()

    def test_cleanup_requires_confirmed_scheduled_reset_and_does_not_use_reset_gpio(self):
        f = Fixture()
        member = f.added().create()
        f.reset_mode = "offline"
        with self.assertRaises(MembershipCleanupError):
            member.close()
        self.assertEqual(len(f.posts("remove")), 1)
        self.assertEqual(f.presses, [3.5])
        self.assertEqual(len(f.gpio.calls), 2)
        self.assertTrue(f.main.membership_operations[-1]["inventory_removed"])
        self.assertFalse(f.main.membership_operations[-1]["retained_member"])

    def test_cleanup_fences_the_main_and_member_boot_review_before_removal(self):
        for change in ("main", "target_race", "config_race"):
            with self.subTest(change=change):
                f = Fixture()
                member = f.added().create()
                if change == "main":
                    f.system["boot_id"] = "9" * 32
                elif change == "target_race":
                    f.change_boot_before_remove = True
                else:
                    f.change_config_before_remove = True
                with self.assertRaises(MembershipCleanupError):
                    member.close()
                self.assertEqual(len(f.posts("remove")), 0 if change == "main" else 3 if change == "config_race" else 1)
                self.assertTrue(any(p["octopus_id"] == f.candidate.identity.device_id
                                    for p in f.system["membership"]["peers"]))


if __name__ == "__main__":
    unittest.main()
