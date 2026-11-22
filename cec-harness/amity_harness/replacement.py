"""Review and apply the shipping replacement workflow to test-owned equipment."""

from copy import deepcopy
import json
import time
from uuid import uuid4

from .components import PowerState
from .discovery import _saved_name
from .dut import AmityHTTPError
from .membership import MembershipConflict
from .removal import _digest, _member_signature, _settings_signature, _without_revisions
from .trace import event


def _name(component, group):
    return {"consortium_id": group, "octopus_id": component.dut.identity.device_id,
            "tentacle_id": component.tentacle, "side": component.direction, "osd_name": component.name}


def _same_port(first, second):
    return first.get("tentacle_id") == second.get("tentacle_id") and first.get("side") == second.get("side")


def _port(name):
    return {key: name.get(key) for key in ("consortium_id", "octopus_id", "tentacle_id", "side")}


def replacement_proposal(configuration, system, previous_id, replacement_id):
    """Plan a replacement as the UI's Replace review does, without overrides.

    The saved equipment of the failed Amity is matched by OSD name against
    the replacement's resolved observations, preferring the same connector.
    Unseen equipment follows equipment that shared its old connector, or
    keeps that connector. Each old connector maps to the single connector
    its equipment now uses. Returns the request body the UI submits (without
    its request ID), the affected activity IDs and whether Replace is enabled.
    """
    peers = system.get("membership", {}).get("peers") or []
    peer = next((row for row in peers if row.get("octopus_id") == replacement_id), {})
    mask = 1 if peer.get("configured_tentacles_mask") is None else peer["configured_tentacles_mask"]
    connectors = [{"tentacle_id": tentacle, "side": side}
                  for tentacle in range(peer.get("supported_tentacles") or 1) if mask & (1 << tentacle)
                  for side in ("front", "back")]
    seen = [row["qualified_name"] for row in system.get("observations") or []
            if row.get("resolved") and (row.get("qualified_name") or {}).get("octopus_id") == replacement_id]
    rows = []
    for component in configuration.get("components") or []:
        # Requests name the consortium that saved names leave to the configuration.
        previous = _saved_name(configuration, component)
        if previous is None or previous["octopus_id"] != previous_id:
            continue
        matches = [name for name in seen if name.get("osd_name") == previous["osd_name"]]
        found = next((name for name in matches if _same_port(name, previous)), matches[0] if matches else None)
        rows.append({"component_id": component.get("component_id"), "previous": previous, "found": found})
    for row in rows:
        if row["found"] is not None:
            row["port"] = row["found"]
            continue
        mate = next((other for other in rows if other["found"] is not None
                     and _same_port(other["previous"], row["previous"])), None)
        row["port"] = mate["found"] if mate is not None else row["previous"]
        if not any(_same_port(connector, row["port"]) for connector in connectors):
            row["port"] = next((connector for connector in connectors
                                if connector["side"] == row["previous"].get("side")),
                               connectors[0] if connectors else None)
        if row["port"] is None:
            raise MembershipConflict("The replacement Amity has no configured connector")
    groups = {}
    for row in rows:
        groups.setdefault(tuple(_port(row["previous"]).values()), []).append(row)
    ports, targets = [], set()
    for group in groups.values():
        target = group[0]["port"]
        key = (target.get("tentacle_id"), target.get("side"))
        if any(not _same_port(row["port"], target) for row in group) or key in targets:
            continue
        targets.add(key)
        previous = _port(group[0]["previous"])
        ports.append({"previous": previous,
                      "replacement": {**previous, "octopus_id": replacement_id,
                                      "tentacle_id": target["tentacle_id"], "side": target["side"]}})
    equipment = {row["component_id"] for row in rows}
    affected = [row.get("activity_id") for row in configuration.get("activities") or []
                if any(row.get(role) in equipment for role in ("display", "source", "audio", "switch_device"))]
    old = next((row for row in peers if row.get("octopus_id") == previous_id), {})
    proposal = {"expected_config_revision": configuration.get("config_revision"),
                "expected_presentation_revision": configuration.get("presentation_revision"),
                "expected_membership_revision": system.get("membership", {}).get("membership_revision"),
                "expected_target_boot_id": old.get("boot_id") or None,
                "previous_octopus_id": previous_id, "replacement_octopus_id": replacement_id,
                "old_hardware_isolated": True, "ports": ports,
                "equipment": [{"component_id": row["component_id"], "previous": row["previous"],
                               "replacement": row["found"] if row["found"] is not None else
                               {**row["previous"], "octopus_id": replacement_id,
                                "tentacle_id": row["port"]["tentacle_id"], "side": row["port"]["side"]},
                               "same_equipment_confirmed": True} for row in rows]}
    enabled = bool(peer.get("enrolled")) and bool(peer.get("ready")) and bool(rows) and bool(ports)
    return deepcopy(proposal), affected, enabled


def _canonical(rows):
    return sorted(json.dumps(row, sort_keys=True) for row in rows)


class ReplacementReview:
    """Keep saved activity intent fixed while its physical recipients move.

    Only unchanged equipment allocated by these fixture activities may move.
    The submitted mapping is the UI's own plan, which must equal the fixture's
    physical moves. After exact saved-data verification, update the fixtures'
    expected physical recipients and cleanup records, without writing
    activities or equipment.
    """

    def __init__(self, previous, replacement, activities, equipment_pairs):
        self.previous, self.replacement = previous, replacement
        self.main, self.bed = previous.main, previous.main._testbed
        self.activities, self.pairs = tuple(activities), tuple(equipment_pairs)
        self.deadline = time.monotonic() + previous.timeout
        self.request_id = uuid4().hex
        self._attempted = self._applied = False
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
        self._fixture_payload = self._fixture_proposal()
        self.payload = self._reviewed(self.before)
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
        config = self._request("/api/config")
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

    def _fixture_proposal(self):
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
            name = _saved_name(self.expected, component)
            if name is None or name["octopus_id"] != self.previous.device_id:
                continue
            if component != created.get(component["component_id"]):
                raise MembershipConflict("Replacement cannot move equipment not allocated by its fixture activities")
            pairs = [(old, new) for old, new in self.pairs if _name(old, self.previous.group) == name]
            if len(pairs) != 1:
                raise MembershipConflict("Replacement must map every saved old equipment selection")
            old, new = pairs[0]
            self.main.last_scan.assert_on(new, new.endpoint)
            target = _name(new, self.previous.group)
            equipment.append({"component_id": component["component_id"], "previous": name,
                              "replacement": target, "same_equipment_confirmed": True})
            port = {key: name[key] for key in ("consortium_id", "octopus_id", "tentacle_id", "side")}
            mapped = {key: target[key] for key in port}
            key = (old.tentacle, old.direction)
            if key in ports and ports[key]["replacement"] != mapped:
                raise MembershipConflict("Equipment mappings disagree on a replacement connector")
            ports[key] = {"previous": port, "replacement": mapped}
            # The saved name keeps only the connection and OSD name.
            component["qualified_name"] = {key: target[key] for key in ("octopus_id", "tentacle_id", "side", "osd_name")}
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

    def _reviewed(self, snapshot):
        """Return the UI's plan, which must equal the fixture's physical moves."""
        proposal, affected, enabled = replacement_proposal(snapshot["config"], snapshot["system"],
                                                           self.previous.device_id, self.replacement.device_id)
        if not enabled:
            raise MembershipConflict("The Replace review would not enable this replacement")
        expected = self._fixture_payload
        if (set(proposal) != set(expected)
                or any(proposal[key] != expected[key] for key in proposal if key not in ("ports", "equipment"))
                or _canonical(proposal["ports"]) != _canonical(expected["ports"])
                or _canonical(proposal["equipment"]) != _canonical(expected["equipment"])
                or len(affected) != len(set(affected)) or set(affected) != set(self.affected)):
            raise AssertionError("The Replace review planned other equipment, connections or activities")
        return proposal

    def apply(self):
        if self._attempted:
            raise RuntimeError("A replacement can be applied only once")
        self.deadline = time.monotonic() + self.previous.timeout
        current = self._snapshot()
        if (current["config"] != self.before["config"]
                or _member_signature(current["system"]) != _member_signature(self.before["system"])):
            raise MembershipConflict("Replacement needs an unchanged reviewed configuration and membership")
        # Like the dialog, submit only when a fresh plan equals the reviewed one.
        if self._reviewed(current) != self.payload:
            raise MembershipConflict("The replacement equipment changed after its review")
        self._attempted = True
        record = self.previous._record("replacement", request_id=self.request_id)
        try:
            try:
                operation = self._request("/api/replacements/apply", {"request_id": self.request_id,
                                                                      **deepcopy(self.payload)})
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
            # Once applied, Replace has removed the old member and moved its
            # equipment. Record both before verifying, so even after a failed
            # check cleanup neither runs Remove again, cascade-deleting the
            # preserved activities, nor prunes by the old equipment records.
            self.previous._closed = True
            self.bed._member_proofs.pop(self.previous.device_id)
            record.update(inventory_removed=True, retained_member=False, reset=operation.get("reset"))
            records = {row["component_id"]: row for row in self.expected["components"]}
            for activity in self.activities:
                activity._new_equipment = {identity: deepcopy(records[identity])
                                           for identity in activity._new_equipment}
            self._assert_saved()
            if operation.get("reset") not in {"scheduled", "offline"}:
                raise AssertionError("Replacement did not resolve the old member's removal")
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
            for activity, selected in selections:
                activity.components = selected
                activity.scan = scan
                activity.refresh_scan(scan)
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
