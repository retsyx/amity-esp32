"""Review and apply the shipping replacement workflow to test-owned equipment."""

from copy import deepcopy
import time
from uuid import uuid4

from .components import PowerState
from .dut import AmityHTTPError
from .membership import MembershipConflict
from .removal import _digest, _member_signature, _settings_signature, _without_revisions
from .trace import event


def _name(component, group):
    return {"consortium_id": group, "octopus_id": component.dut.identity.device_id,
            "tentacle_id": component.tentacle, "side": component.direction, "osd_name": component.name}


class ReplacementReview:
    """Keep saved activity intent fixed while its physical recipients move.

    Only unchanged equipment allocated by these fixture activities may move.
    After exact saved-data verification, update the fixtures' expected physical
    recipients and cleanup records, without writing activities or equipment.
    """

    def __init__(self, previous, replacement, activities, equipment_pairs):
        self.previous, self.replacement = previous, replacement
        self.main, self.bed = previous.main, previous.main._testbed
        self.activities, self.pairs = tuple(activities), tuple(equipment_pairs)
        self.deadline = time.monotonic() + previous.timeout
        self.request_id = uuid4().hex
        self._previewed = self._applied = False
        proofs = getattr(self.bed, "_member_proofs", {})
        if (previous is replacement or replacement.main is not self.main
                or not self.bed._entered or self.bed._closed
                or any(not member._created or member._closed or member.session is not self.bed.session
                       or proofs.get(member.device_id) is not member
                       for member in (previous, replacement))
                or not self.activities or len(set(self.activities)) != len(self.activities)
                or any(activity not in self.bed._activities or activity.dut is not self.main
                       or not activity._created or activity._closed or activity._needs_standby
                       for activity in self.activities)):
            raise MembershipConflict("Replacement needs verified fixture members and saved standby activities")
        if (not self.pairs or len({old for old, new in self.pairs}) != len(self.pairs)
                or len({new for old, new in self.pairs}) != len(self.pairs)):
            raise MembershipConflict("Replacement needs distinct old and new equipment pairs")
        for old, new in self.pairs:
            if (old.dut is not previous.candidate or new.dut is not replacement.candidate
                    or type(old) is not type(new) or old.name != new.name
                    or any(component._session is not self.bed.session
                           or component not in self.bed.components for component in (old, new))):
                raise MembershipConflict("Replacement must map the same fixture equipment to the verified new owner")
            old.assert_power(PowerState.OFF)
            new.assert_power(PowerState.STANDBY)
        self.before = self._snapshot()
        self.expected = deepcopy(self.before["config"])
        self.payload = self._proposal()
        self.proof = {"kind": "amity_replacement", "request_id": self.request_id,
                      "device_id": previous.device_id, "survivor": replacement.device_id,
                      "main_boot": previous.boot, "member_boot": previous.member_boot,
                      "survivor_boot": replacement.member_boot,
                      "activity_ids": [activity.activity_id for activity in self.activities],
                      "component_ids": [row["component_id"] for row in self.payload["equipment"]],
                      "expected_config_sha256": _digest(_without_revisions(self.expected))}
        if not hasattr(self.bed, "control_observations"):
            self.bed.control_observations = []
        self.bed.control_observations.append(self.proof)

    def _request(self, path, data=None):
        return self.previous._request(path, data, deadline=self.deadline)

    def _settings(self, owner, boot, supported):
        while True:
            settings = self._request("/api/devices/" + owner + "/settings")
            if settings.get("status") != "reading":
                if not self.previous._settings_match(settings, {"boot_id": boot, "supported_tentacles": supported}):
                    raise MembershipConflict("Replacement lost the current owner's settings or boot")
                return settings
            time.sleep(min(.25, self.previous._remaining(self.deadline)))

    def _snapshot(self):
        system = self.previous._system()
        config = self._request("/api/backup")
        editor = self.previous._observe_http("/api/activity-editor", self.main.get_configuration)
        state = self.previous._observe_http("/api/activity-editor/status", self.main.get_state)
        if (config.get("controller_octopus_id") != self.main.identity.device_id
                or config.get("consortium_id") != self.previous.group
                or state.get("expected_boot_id") != self.previous.boot
                or state.get("current_activity") != -1 or state.get("activity_settling") is not False
                or system.get("configuration_pending") is not False
                or any(config.get(key) != system.get(applied) for key, applied in
                       (("config_revision", "applied_config_revision"),
                        ("presentation_revision", "applied_presentation_revision")))
                or any(view.get("expected_" + key) != config.get(key)
                       for view in (editor, state) for key in ("config_revision", "presentation_revision"))):
            raise MembershipConflict("Replacement needs an applied configuration on the original main in standby")
        peer = self.previous._active(system).get(self.replacement.device_id, {})
        if (peer.get("boot_id") != self.replacement.member_boot or peer.get("ready") is not True
                or peer.get("hostname") != self.replacement.name):
            raise MembershipConflict("Replacement lost its verified new member")
        settings = self._settings(self.replacement.device_id, self.replacement.member_boot,
                                  peer["supported_tentacles"])
        owner = next(row for row in config["octopuses"]
                     if row["octopus_id"] == self.main.identity.device_id)
        main_settings = self._settings(self.main.identity.device_id, self.previous.boot,
                                       owner["supported_tentacles"])
        return deepcopy({"config": config, "system": system, "editor": editor,
                         "settings": settings, "main_settings": main_settings})

    def _proposal(self):
        config, system, editor = (self.before[key] for key in ("config", "system", "editor"))
        old_peer = self.previous._active(system).get(self.previous.device_id, {})
        if old_peer.get("boot_id") != self.previous.member_boot or old_peer.get("ready") is not True:
            raise MembershipConflict("Replacement lost the reviewed old member boot")
        created = {}
        for activity in self.activities:
            if [row for row in editor["activities"] if row.get("activity_id") == activity.activity_id] != [activity._created_row]:
                raise MembershipConflict("Replacement cannot change an edited or missing fixture activity")
            created.update(activity._new_equipment)
        equipment, ports, matched = [], {}, set()
        for component in self.expected["components"]:
            for index, name in enumerate(component["qualified_names"]):
                if name["octopus_id"] != self.previous.device_id:
                    continue
                if component != created.get(component["component_id"]):
                    raise MembershipConflict("Replacement cannot move equipment not allocated by its fixture activities")
                pairs = [(old, new) for old, new in self.pairs if _name(old, self.previous.group) == name]
                if len(pairs) != 1:
                    raise MembershipConflict("Replacement must map every saved old equipment selection")
                old, new = pairs[0]
                self.main.last_scan.assert_on(new, new.endpoint)
                target = _name(new, self.previous.group)
                equipment.append({"component_id": component["component_id"], "previous": deepcopy(name),
                                  "replacement": target, "same_equipment_confirmed": True})
                port = {key: name[key] for key in ("consortium_id", "octopus_id", "tentacle_id", "side")}
                mapped = {key: target[key] for key in port}
                key = (old.tentacle, old.direction)
                if key in ports and ports[key]["replacement"] != mapped:
                    raise MembershipConflict("Equipment mappings disagree on a replacement connector")
                ports[key] = {"previous": port, "replacement": mapped}
                component["qualified_names"][index] = target
                matched.add(old)
        if matched != {old for old, new in self.pairs}:
            raise MembershipConflict("Replacement mapping includes unsaved equipment")
        affected = set(self.previous._affected_activities(config, {self.previous.device_id}))
        if not affected or not affected <= {activity.activity_id for activity in self.activities}:
            raise MembershipConflict("Replacement affects an activity outside the fixture")
        self.affected = sorted(affected)
        self.expected["octopuses"] = [row for row in config["octopuses"]
                                      if row["octopus_id"] != self.previous.device_id]
        return {"expected_config_revision": config["config_revision"],
                "expected_presentation_revision": config["presentation_revision"],
                "expected_membership_revision": system["membership"]["membership_revision"],
                "expected_target_boot_id": self.previous.member_boot,
                "previous_octopus_id": self.previous.device_id,
                "replacement_octopus_id": self.replacement.device_id,
                "old_hardware_isolated": True, "ports": list(ports.values()), "equipment": equipment}

    def preview(self):
        if self._previewed or self._applied:
            raise RuntimeError("This replacement was already reviewed")
        result = self._request("/api/replacements/preview", {**deepcopy(self.payload), "request_id": uuid4().hex})
        if (set(result.get("affected_activity_ids", [])) != set(self.affected)
                or _without_revisions(result.get("configuration", {})) != _without_revisions(self.expected)):
            raise AssertionError("Replacement preview changed saved activity or equipment intent")
        after = self._snapshot()
        if (after["config"] != self.before["config"]
                or _member_signature(after["system"]) != _member_signature(self.before["system"])
                or any(_settings_signature(after[key]) != _settings_signature(self.before[key])
                       for key in ("settings", "main_settings"))):
            raise AssertionError("Replacement preview mutated configuration, membership or settings")
        self._previewed = True
        self.proof.update(configuration_unchanged=True, membership_unchanged=True, settings_unchanged=True)
        return self

    def apply(self):
        if not self._previewed or self._applied:
            raise RuntimeError("Preview this replacement exactly once before applying")
        self.deadline = time.monotonic() + self.previous.timeout
        current = self._snapshot()
        if (current["config"] != self.before["config"]
                or _member_signature(current["system"]) != _member_signature(self.before["system"])):
            raise MembershipConflict("Replacement needs an unchanged reviewed configuration and membership")
        record = self.previous._record("replacement", request_id=self.request_id)
        try:
            try:
                operation = self._request("/api/replacements/apply", {**deepcopy(self.payload), "request_id": self.request_id})
            except (OSError, TimeoutError, AmityHTTPError) as error:
                if isinstance(error, AmityHTTPError) and 400 <= error.status < 500 and error.status != 408:
                    raise
                operation = self._request("/api/operations/" + self.request_id)
            while True:
                if operation.get("operation_id") != self.request_id or operation.get("boot_id") != self.previous.boot:
                    raise MembershipConflict("Replacement operation identity or main boot changed")
                stage = operation.get("stage")
                record["operation_stage"] = stage
                if stage == "applied":
                    break
                if stage not in {"admitted", "preparing", "executing", "configuring", "activating", "stopping"}:
                    raise AssertionError("Replacement operation did not apply successfully")
                time.sleep(min(.25, self.previous._remaining(self.deadline)))
                operation = self._request("/api/operations/" + self.request_id)
            self._assert_saved()
            if operation.get("reset") not in {"scheduled", "offline"}:
                raise AssertionError("Replacement did not resolve the old member's removal")
            # The old member was removed by Replace, so ordinary Remove must
            # not run again or cascade-delete the activities we just preserved.
            self.previous._closed = True
            self.bed._member_proofs.pop(self.previous.device_id)
            record.update(inventory_removed=True, retained_member=False, reset=operation["reset"])
            scan = self.main.scan_hdmi()
            replacements = dict(self.pairs)
            selections = []
            for activity in self.activities:
                selected = {role: replacements.get(component, component)
                            for role, component in activity.components.items()}
                for component in selected.values():
                    if component is not None:
                        scan.assert_on(component, component.endpoint)
                selections.append((activity, selected))
            records = {row["component_id"]: row for row in self.expected["components"]}
            for activity, selected in selections:
                activity.components = selected
                activity.scan = scan
                activity.refresh_scan(scan)
                activity._new_equipment = {identity: deepcopy(records[identity])
                                           for identity in activity._new_equipment}
            self._applied = True
            self.proof.update(operation_stage="applied", reset=operation["reset"], removed=True,
                              affected_activity_ids=self.affected)
            event("membership.replacement.passed", dut=self.main.name, device_id=self.previous.device_id,
                  replacement=self.replacement.device_id, activity_ids=self.proof["activity_ids"])
            return self
        except BaseException as error:
            record["error_type"] = type(error).__name__
            raise

    def _assert_saved(self):
        after = self._snapshot()
        config, system = after["config"], after["system"]
        if _without_revisions(config) != _without_revisions(self.expected):
            raise AssertionError("Replacement did not retain the exact saved activity configuration")
        if int(config["config_revision"]) <= int(self.before["config"]["config_revision"]):
            raise AssertionError("Replacement did not commit a new configuration revision")
        before_peers = self.previous._active(self.before["system"])
        expected_peers = {identity: peer for identity, peer in before_peers.items()
                          if identity != self.previous.device_id}
        actual = self.previous._active(system)
        fields = ("octopus_id", "hostname", "boot_id", "enrolled", "provisional", "ready")
        if ({identity: {key: row.get(key) for key in fields} for identity, row in actual.items()}
                != {identity: {key: row.get(key) for key in fields} for identity, row in expected_peers.items()}):
            raise AssertionError("Replacement changed another member's identity, boot or readiness")
        expected_names = [row for row in self.before["system"].get("named_devices", [])
                          if row.get("octopus_id") != self.previous.device_id]
        if (system.get("named_devices", []) != expected_names
                or any(row.get(field, {}).get("octopus_id") == self.previous.device_id
                       for row in system.get("observations", []) for field in ("qualified_name", "runtime_address"))):
            raise AssertionError("Replacement retained old owner metadata or changed another saved name")
        removed = self._request("/api/devices/" + self.previous.device_id + "/settings")
        if removed.get("status") != "offline" or removed.get("settings") is not None:
            raise AssertionError("Replacement retained old owner settings")
        if any(_settings_signature(after[key]) != _settings_signature(self.before[key])
               for key in ("settings", "main_settings")):
            raise AssertionError("Replacement changed unrelated adapter settings")
        self.proof.update(exact_configuration=True, inventory_removed=True, settings_purged=True,
                          unrelated_settings_preserved=True, preserved=True,
                          actual_config_sha256=_digest(_without_revisions(config)))
        return after

    def assert_retained(self):
        if not self._applied:
            raise RuntimeError("Apply the replacement before checking retained configuration")
        self.deadline = time.monotonic() + self.previous.timeout
        self._assert_saved()
        return self
