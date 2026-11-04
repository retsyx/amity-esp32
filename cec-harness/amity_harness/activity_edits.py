"""Edit inactive fixture activities without changing the current playback session."""

from copy import deepcopy
import time

from .activity import ActivityConflict
from .dut import AmityHTTPError
from .removal import _digest, _without_revisions
from .transitions import _WireObservation


class ActivityEditObservation(_WireObservation):
    """Observe edit side effects while allowing the scenario's explicit keys."""

    def __init__(self, *components, keys):
        super().__init__(components)
        self.keys = frozenset(keys)
        if (not self.keys or any(isinstance(key, bool) or not isinstance(key, int) for key in self.keys)
                or not self.keys <= {0x00, 0x01, 0x02, 0x03, 0x04, 0x41, 0x42, 0x43}):
            raise ValueError("An activity edit observation permits only its explicit navigation and volume keys")
        self._key_operands = {bytes([key]) for key in self.keys}

    def assert_preserved(self):
        # Exercise keys before this receive-coverage tail: waiting first could
        # conceal a configuration revision that temporarily broke remote keys.
        through = time.monotonic_ns() + int(min(component._power_timeout for component in self.components) * 1e9)
        session, started, ended, wires, frames = self._complete(self._started, through=through)
        for component, frame in frames:
            routing = frame.opcode in (0x80, 0x81, 0x82, 0x86, 0x9d)
            unexpected_key = frame.opcode == 0x44 and frame.operands not in self._key_operands
            malformed_release = frame.opcode == 0x45 and bool(frame.operands)
            if self._power(component, frame) is not None or routing or unexpected_key or malformed_release:
                raise AssertionError("Activity editing sent power, routing or an unrequested key on a fixture wire")
        proof = {"kind": "wire_activity_edit_preserved", "timestamp_ns": time.monotonic_ns(),
                 "since_monotonic_ns": started, "observation_end_monotonic_ns": ended,
                 "idle_receive_since_ns": {str(gpio): session._wire_idle_since_ns[gpio] for gpio in wires},
                 "observed_wires": sorted(wires)}
        for component in self.components:
            component._routing_checks.append(dict(proof))
            component._trace("component.activity_edit.assert_passed", **proof)
        self._checked = True
        return self


class ActiveActivityEdits:
    """Use the ordinary editor while fencing saved intent and current playback."""

    def __init__(self, active):
        self.active, self.dut, self.bed = active, active.dut, active._testbed
        if (active not in self.bed._activities or not active._created or active._closed
                or not self.bed._entered or self.bed._closed):
            raise ActivityConflict("Activity edits need a live saved fixture activity")
        self._snapshot()

    def _snapshot(self):
        document, state = self.active._view()
        full = self.active._full_configuration(document)
        index = state.get("current_activity")
        rows = document["activities"]
        if (type(index) is not int or not 0 <= index < len(rows)
                or rows[index] != self.active._created_row
                or rows[index].get("activity_id") != self.active.activity_id
                or state.get("activity_settling") is not False
                or state.get("activity_error") != ""
                or not self.active._saved_selections_match(document)):
            raise ActivityConflict("The exact fixture activity must remain current during editing")
        return deepcopy({"editor": document, "state": state, "config": full})

    def _state(self, snapshot):
        state, rows = snapshot["state"], snapshot["editor"]["activities"]
        fields = ("expected_boot_id", "expected_state_revision", "activity_generation",
                  "activity_settling", "activity_error", "discovery_generation", "discovering", "devices")
        return {"activity_id": rows[state["current_activity"]]["activity_id"],
                **{key: state.get(key) for key in fields}}

    def _preserved(self, before, after):
        if self._state(after) != self._state(before):
            raise AssertionError("Editing activity configuration changed playback, its revision or discovery cache")

    def _proof(self, **values):
        if not hasattr(self.bed, "control_observations"):
            self.bed.control_observations = []
        self.bed.control_observations.append({"kind": "active_activity_edits",
                                              "activity_ids": [self.active.activity_id], **values})

    def assert_active_rejected(self, *, delete=False):
        """Reject an active-row name edit or deletion before operation admission."""
        before = self._snapshot()
        rows = deepcopy(before["editor"]["activities"])
        if delete:
            rows = [row for row in rows if row["activity_id"] != self.active.activity_id]
        else:
            row = next(row for row in rows if row["activity_id"] == self.active.activity_id)
            row["name"] = "Changed active activity"
        payload = self.active._editor_payload(before["editor"], rows)
        try:
            self.dut.request("/api/activity-editor", payload)
        except AmityHTTPError as error:
            if error.status != 409 or error.code != "active_activity":
                raise AssertionError("The active activity was not rejected for its current playback") from error
        else:
            raise AssertionError("The active activity edit or deletion was accepted")
        try:
            self.dut.request("/api/operations/" + payload["request_id"])
        except AmityHTTPError as error:
            if error.status != 404:
                raise
        else:
            raise AssertionError("The rejected active edit entered the operation ledger")
        after = self._snapshot()
        if after["config"] != before["config"] or after["editor"] != before["editor"]:
            raise AssertionError("A rejected active edit changed saved activity configuration")
        self._preserved(before, after)
        self._proof(request_id=payload["request_id"], http_status=409, error_code="active_activity",
                    operation_not_admitted=True, configuration_unchanged=True, preserved=True,
                    unchanged_config_sha256=_digest(before["config"]))
        return self

    def edit_other(self, other, *, switch_input):
        """Change a saved inactive input and reorder its row around the active row."""
        if (other is self.active or other.dut is not self.dut or other._testbed is not self.bed
                or other not in self.bed._activities or not other._created or other._closed
                or other._needs_standby or other.components["switch"] is None
                or type(switch_input) is not int or not 0 <= switch_input <= 255):
            raise ActivityConflict("Edit a distinct saved inactive fixture's numeric switch input")
        before = self._snapshot()
        rows = deepcopy(before["editor"]["activities"])
        selected = [row for row in rows if row.get("activity_id") == other.activity_id]
        if selected != [other._created_row]:
            raise ActivityConflict("The inactive fixture activity changed before editing")
        selected[0]["switch"]["input"] = switch_input
        # A session follows its activity identity even when another row's edit
        # moves its projected UI index. No activity is selected again here.
        rows.sort(key=lambda row: row["activity_id"] != self.active.activity_id)
        order = [row["activity_id"] for row in rows]
        previous_ids = {row["activity_id"] for row in before["editor"]["activities"]}
        expected = deepcopy(before["config"])
        target = next(row for row in expected["activities"] if row["activity_id"] == other.activity_id)
        target["switch_input"] = switch_input
        by_id = {row["activity_id"]: row for row in expected["activities"]}
        # Saved activity records retain their canonical identity order. The
        # explicit presentation arrays carry the editor's row ordering.
        expected["activity_order"] = order + [identity for identity in expected["activity_order"]
                                              if identity not in previous_ids]
        for zone in expected["zones"]:
            zone["activity_order"] = ([identity for identity in order
                                        if by_id[identity]["zone_id"] == zone["zone_id"]]
                                      + [identity for identity in zone["activity_order"] if identity not in previous_ids])
        payload = self.active._editor_payload(before["editor"], rows)
        trace = self.active._record("edit_other", request_id=payload["request_id"])
        self.active._write("/api/activity-editor", payload, time.monotonic() + self.active.timeout,
                           trace, stage="applied")
        after = self._snapshot()
        if (_without_revisions(after["config"]) != _without_revisions(expected)
                or after["editor"]["activities"] != rows):
            raise AssertionError("The inactive edit changed unrelated saved activity configuration")
        if int(after["config"]["config_revision"]) <= int(before["config"]["config_revision"]):
            raise AssertionError("The inactive edit did not commit its switch input")
        self._preserved(before, after)
        other._created_row = deepcopy(selected[0])
        other.switch_input = switch_input
        self._proof(request_id=payload["request_id"], exact_configuration=True, preserved=True,
                    expected_config_sha256=_digest(_without_revisions(expected)),
                    actual_config_sha256=_digest(_without_revisions(after["config"])))
        return self
