"""Exercise the shipping Remove confirmation against test-owned saved data."""

from copy import deepcopy
import hashlib
import json
import time
from uuid import uuid4

from .discovery import _identifier
from .dut import AmityHTTPError
from .membership import MembershipConflict
from .trace import event


ROLES = ("source", "display", "audio", "switch_device")


def _digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def _without_revisions(config):
    return {key: value for key, value in config.items()
            if key not in ("config_revision", "presentation_revision")}


def _projection(config, owner, equipment, affected):
    """Exact saved-data effect promised by the existing Remove dialog."""
    result = deepcopy(config)
    result["octopuses"] = [row for row in result["octopuses"] if row["octopus_id"] != owner]
    result["components"] = [row for row in result["components"] if row["component_id"] not in equipment]
    result["activities"] = [row for row in result["activities"] if row["activity_id"] not in affected]
    if "activity_order" in result:
        result["activity_order"] = [identity for identity in result["activity_order"] if identity not in affected]
    for zone in result["zones"]:
        zone["activity_order"] = [identity for identity in zone["activity_order"] if identity not in affected]
    for slot in result["external_slots"]:
        if slot["activity_id"] in affected:
            slot.update(tombstone=True, hidden=False, alias="")
    return result


def _member_signature(system):
    fields = ("octopus_id", "hostname", "boot_id", "enrolled", "provisional", "ready")
    return {"membership_revision": system["membership"]["membership_revision"],
            "peers": [{key: row.get(key) for key in fields} for row in system["membership"]["peers"]],
            "names": deepcopy(system.get("named_devices", []))}


def _settings_signature(settings):
    body = deepcopy(settings.get("settings"))
    if isinstance(body, dict):
        for adapter in body.get("adapters", []):
            for key in ("observed_front_physical_address", "effective_front_physical_address"):
                adapter.pop(key, None)
    return {"status": settings.get("status"), "settings": body}


class RemovalReview:
    """One review, one confirmation, and exact cascade/preservation evidence.

    Normal member cleanup stays conservative. This explicit operation admits
    only unchanged registered fixture rows and their own equipment records.
    """

    def __init__(self, member, activities):
        self.member, self.main, self.bed = member, member.main, member.main._testbed
        self.activities, self.request_id, self._used = tuple(activities), uuid4().hex, False
        if (not member._created or member._closed or member._remove_attempted
                or not self.bed._entered or self.bed._closed or self.bed.session is not member.session
                or not self.activities or len({id(activity) for activity in self.activities}) != len(self.activities)):
            raise MembershipConflict("Removal review needs a live test member and distinct fixture activities")
        if (any(row.get("octopus_id") == member.device_id for row in member._baseline.get("octopuses", []))
                or any(row.get("octopus_id") == member.device_id
                       for row in member._initial_system.get("named_devices", []))):
            raise MembershipConflict("Explicit test removal cannot own a pre-existing saved member record")
        self.deadline = time.monotonic() + member.timeout
        member._record("review_removal", request_id=self.request_id)
        self.before = self._snapshot()
        self.equipment, self.affected, self.roles = self._owned_rows(self.before)
        self._payload = {"request_id": self.request_id, "octopus_id": member.device_id, "confirm": True,
                        "expected_membership_revision": self.before["system"]["membership"]["membership_revision"],
                        "expected_target_boot_id": member.member_boot,
                        "expected_config_revision": self.before["config"]["config_revision"],
                        "expected_presentation_revision": self.before["config"]["presentation_revision"],
                        "affected_activity_ids": self.affected}
        self.proof = {"kind": "removal_cascade", "request_id": self.request_id,
                      "device_id": member.device_id, "main_boot": member.boot,
                      "member_boot": member.member_boot, "affected_activity_ids": list(self.affected),
                      "affected_activities": [{"activity_id": row["activity_id"], "name": row["label"]}
                                              for row in self.before["config"]["activities"]
                                              if row["activity_id"] in self.affected],
                      "role_activities": deepcopy(self.roles),
                      "reviewed_config_revision": self.payload["expected_config_revision"],
                      "reviewed_presentation_revision": self.payload["expected_presentation_revision"],
                      "reviewed_membership_revision": self.payload["expected_membership_revision"]}
        if not hasattr(self.bed, "control_observations"):
            self.bed.control_observations = []
        self.bed.control_observations.append(self.proof)

    @property
    def payload(self):
        """A copy for reviewing evidence; confirmation retains the frozen body."""
        return deepcopy(self._payload)

    def _request(self, path, data=None):
        return self.member._request(path, data, deadline=self.deadline)

    def _snapshot(self, *, removed=False):
        system = self.member._system()
        config = self._request("/api/backup")
        editor = self.member._observe_http("/api/activity-editor", self.main.get_configuration)
        state = self.member._observe_http("/api/activity-editor/status", self.main.get_state)
        if (not isinstance(config, dict) or config.get("controller_octopus_id") != self.main.identity.device_id
                or config.get("consortium_id") != self.member.group
                or any(not isinstance(config.get(key), list)
                       for key in ("octopuses", "components", "activities", "zones", "external_slots"))
                or editor.get("expected_config_revision") != config.get("config_revision")
                or editor.get("expected_presentation_revision") != config.get("presentation_revision")
                or state.get("expected_config_revision") != config.get("config_revision")
                or state.get("expected_presentation_revision") != config.get("presentation_revision")
                or state.get("expected_boot_id") != self.member.boot
                or state.get("current_activity") != -1 or state.get("activity_settling") is not False
                or system.get("configuration_pending") is not False
                or system.get("applied_config_revision") != config.get("config_revision")
                or system.get("applied_presentation_revision") != config.get("presentation_revision")):
            raise MembershipConflict("Removal review lost its applied configuration, standby or main boot")
        settings = self._request("/api/devices/" + self.member.device_id + "/settings")
        if not removed:
            peer = self.member._preserved_members(system, added=True)[self.member.device_id]
            if (peer.get("boot_id") != self.member.member_boot or peer.get("hostname") != self.member.name
                    or peer.get("ready") is not True):
                raise MembershipConflict("Removal review lost the exact ready member boot/settings")
            while settings.get("status") == "reading":
                time.sleep(min(.25, self.member._remaining(self.deadline)))
                fresh = self.member._system()
                if (_member_signature(fresh) != _member_signature(system)
                        or fresh.get("configuration_pending") is not False
                        or fresh.get("applied_config_revision") != config["config_revision"]
                        or fresh.get("applied_presentation_revision") != config["presentation_revision"]):
                    raise MembershipConflict("Member or configuration changed while reading settings")
                system = fresh
                settings = self._request("/api/devices/" + self.member.device_id + "/settings")
            if not self.member._settings_match(settings, peer):
                raise MembershipConflict("Removal review lost the exact ready member boot/settings")
        main_settings = self._request("/api/devices/" + self.main.identity.device_id + "/settings")
        main_owners = [row for row in config["octopuses"] if row.get("octopus_id") == self.main.identity.device_id]
        if (len(main_owners) != 1 or not self.member._settings_match(main_settings,
                {"boot_id": self.member.boot, "supported_tentacles": main_owners[0]["supported_tentacles"]})):
            raise MembershipConflict("The unrelated main adapter settings are not proven on the current boot")
        return {"config": deepcopy(config), "editor": deepcopy(editor), "system": deepcopy(system),
                "settings": deepcopy(settings), "main_settings": deepcopy(main_settings)}

    def _owned_rows(self, snapshot):
        config, editor = snapshot["config"], snapshot["editor"]
        allowed = set()
        created_equipment = {}
        for activity in self.activities:
            if (activity not in getattr(self.bed, "_activities", ()) or activity._testbed is not self.bed
                    or activity.dut is not self.main
                    or activity.boot != self.member.boot or not activity._created or activity._closed
                    or getattr(activity, "_needs_standby", False) or not _identifier(activity.activity_id)):
                raise MembershipConflict("Cascade removal requires unchanged, saved-only fixture activities")
            rows = [row for row in editor.get("activities", []) if row.get("activity_id") == activity.activity_id]
            if len(rows) != 1 or rows[0] != activity._created_row:
                raise MembershipConflict("A reviewed fixture activity changed")
            full = [row for row in config["activities"] if row.get("activity_id") == activity.activity_id]
            if len(full) != 1 or full[0].get("label") != rows[0].get("name"):
                raise MembershipConflict("A reviewed fixture activity is missing or ambiguous")
            for role in ROLES:
                selected = rows[0].get("switch", {}).get("device", "") if role == "switch_device" and rows[0].get("switch") else rows[0].get(role, "")
                expected = selected.removeprefix("component:") if isinstance(selected, str) and selected else None
                if full[0].get(role) != expected:
                    raise MembershipConflict("A reviewed fixture equipment selection changed")
            if full[0].get("switch_input", 0) != (rows[0].get("switch") or {}).get("input", 0):
                raise MembershipConflict("A reviewed fixture switch input changed")
            allowed.add(activity.activity_id)
            created_equipment.update(getattr(activity, "_new_equipment", {}))
        owned = self.member._owned_components(config, {self.member.device_id})
        created = {identity: row for identity, row in created_equipment.items()
                   if any(name.get("octopus_id") == self.member.device_id for name in row.get("qualified_names", []))}
        if not owned or owned != set(created) or any(row != created[row["component_id"]]
                for row in config["components"] if row.get("component_id") in owned):
            raise MembershipConflict("Removal would delete equipment not owned unchanged by these fixtures")
        affected = self.member._affected_activities(config, {self.member.device_id})
        if len(affected) != len(set(affected)) or set(affected) != allowed:
            raise MembershipConflict("Removal would delete an unreviewed activity or miss a fixture activity")
        roles = {role: [row["activity_id"] for row in config["activities"] if row["activity_id"] in allowed
                        and row.get(role) in owned] for role in ROLES}
        if any(not identities for identities in roles.values()):
            raise MembershipConflict("Cascade fixtures must cover source, display, audio and switch independently")
        return owned, affected, roles

    def _consume(self):
        if self._used:
            raise RuntimeError("A removal review can be confirmed only once")
        self._used = True
        self.deadline = time.monotonic() + self.member.timeout

    def assert_stale_rejected(self):
        self._consume()
        before = self._snapshot()
        self._owned_rows(before)
        if (before["config"]["config_revision"], before["config"]["presentation_revision"]) == (
                self.payload["expected_config_revision"], self.payload["expected_presentation_revision"]):
            raise AssertionError("Change unrelated fixture configuration after reviewing removal")
        self.member._remove_id, self.member._remove_attempted = self.request_id, True
        try:
            self.member._remove_operation = self._request("/api/membership/remove", self.payload)
        except AmityHTTPError as error:
            if 400 <= error.status < 500 and error.status != 408:
                self.member._remove_attempted = False
                self.member._remove_id = None
            if error.status != 409 or error.code != "stale_removal":
                raise AssertionError("The stale confirmation was not rejected for stale removal") from error
        else:
            raise AssertionError("The stale removal confirmation was accepted")
        try:
            self._request("/api/operations/" + self.request_id)
        except AmityHTTPError as error:
            if error.status != 404:
                raise
        else:
            raise AssertionError("The rejected stale removal entered the operation ledger")
        after = self._snapshot()
        if (before["config"] != after["config"]
                or _member_signature(before["system"]) != _member_signature(after["system"])
                or _settings_signature(before["settings"]) != _settings_signature(after["settings"])
                or _settings_signature(before["main_settings"]) != _settings_signature(after["main_settings"])):
            raise AssertionError("Rejected confirmation changed saved data, membership or settings")
        self.proof.update(stale_rejected=True, http_status=409, error_code="stale_removal",
                          operation_not_admitted=True, configuration_unchanged=True,
                          membership_unchanged=True, settings_unchanged=True,
                          unchanged_config_sha256=_digest(before["config"]))
        event("membership.removal.stale_rejected", dut=self.main.name, device_id=self.member.device_id,
              request_id=self.request_id, http_status=409)
        return self

    def confirm(self):
        self._consume()
        current = self._snapshot()
        self._owned_rows(current)
        if (current["config"] != self.before["config"]
                or _member_signature(current["system"]) != _member_signature(self.before["system"])):
            raise MembershipConflict("Removal needs a fresh unchanged review")
        record = self.member._record("cleanup", retained_member=True)
        self.member._remove_id, self.member._remove_attempted = self.request_id, True
        try:
            try:
                self.member._remove_operation = self._request("/api/membership/remove", deepcopy(self.payload))
            except AmityHTTPError as error:
                if 400 <= error.status < 500 and error.status != 408:
                    self.member._remove_attempted = False
                    self.member._remove_id = None
                    raise
                self.member._remove_operation = self._request("/api/operations/" + self.request_id)
            except (OSError, TimeoutError):
                # Unknown outcomes reconcile this same administrative request;
                # neither this helper nor normal cleanup resubmits Remove.
                self.member._remove_operation = self._request("/api/operations/" + self.request_id)
            operation = self.member._wait_operation(self.member._remove_operation, self.request_id,
                                                     deadline=self.deadline, record=record)
            record["reset"] = operation.get("reset")
            after = self._snapshot(removed=True)
            expected = _projection(self.before["config"], self.member.device_id, self.equipment, set(self.affected))
            if _without_revisions(after["config"]) != _without_revisions(expected):
                raise AssertionError("Remove did not apply exactly the reviewed cascade or preserve unrelated data")
            if int(after["config"]["config_revision"]) <= int(self.before["config"]["config_revision"]):
                raise AssertionError("Removal did not commit a new configuration revision")
            system = after["system"]
            self.member._preserved_members(system, added=False)
            if any(row.get("octopus_id") == self.member.device_id for row in system["membership"]["peers"]):
                raise AssertionError("The removed member remains in membership records")
            expected_names = [row for row in self.before["system"].get("named_devices", [])
                              if row.get("octopus_id") != self.member.device_id]
            if system.get("named_devices", []) != expected_names:
                raise AssertionError("Remove retained the saved name or changed unrelated device names")
            if any(row.get("qualified_name", {}).get("octopus_id") == self.member.device_id
                   for row in system.get("observations", [])):
                raise AssertionError("The removed owner's observations remain in the main inventory")
            if after["settings"].get("status") != "offline" or after["settings"].get("settings") is not None:
                raise AssertionError("The removed member's settings cache was retained")
            if _settings_signature(after["main_settings"]) != _settings_signature(self.before["main_settings"]):
                raise AssertionError("Remove changed unrelated main adapter settings")
            if operation.get("reset") != "scheduled":
                raise AssertionError("The removed member did not acknowledge its scheduled factory reset")
            record.update(inventory_removed=True, retained_member=False)
            self.proof.update(removed=True, operation_stage="complete", reset="scheduled",
                              exact_configuration=True, deleted_activity_ids=list(self.affected),
                              deleted_component_ids=sorted(self.equipment),
                              preserved_activity_ids=[row["activity_id"] for row in expected["activities"]],
                              expected_config_sha256=_digest(_without_revisions(expected)),
                              actual_config_sha256=_digest(_without_revisions(after["config"])),
                              inventory_removed=True, settings_purged=True, unrelated_settings_preserved=True)
            proofs = getattr(self.bed, "_member_proofs", {})
            if proofs.get(self.member.device_id) is self.member:
                del proofs[self.member.device_id]
            self.member._closed = True
            event("membership.removal.cascade_passed", dut=self.main.name, device_id=self.member.device_id,
                  request_id=self.request_id, affected_activity_ids=list(self.affected), reset="scheduled")
            return self.member
        except BaseException as error:
            record["error_type"] = type(error).__name__
            raise
