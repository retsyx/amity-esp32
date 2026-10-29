"""Temporary activities through the existing authenticated activity-editor UI.

No firmware test API or restore operation is used. Cleanup merges against the
latest configuration, removes only this activity, and prunes only unchanged,
unreferenced equipment records allocated while creating its fixture selections.
"""

from copy import deepcopy
from dataclasses import dataclass, field
import time
from uuid import uuid4

from .discovery import _ObservedEquipment, _fresh_discovery, _identifier, _observed_matches
from .dut import AmityHTTPError, _positive_seconds


class ActivityConflict(RuntimeError):
    """A fresh editor/configuration snapshot changed before it could be used."""


class ActivityCleanupError(RuntimeError):
    def __init__(self, errors):
        self.errors = tuple(errors)
        super().__init__("Activity cleanup failed: " + ", ".join(type(error).__name__ for error in errors))


@dataclass
class ActivityAction:
    """Physical-wire boundary for one accepted start or standby operation."""

    dut: object = field(repr=False)
    session: object = field(repr=False)
    started_ns: int
    boot: str
    initiators: dict
    kind: str
    request_id: str
    activity_id: str | None
    operation: dict = field(default_factory=dict)
    state: dict = field(default_factory=dict)

    @property
    def started(self):
        return self.started_ns


_OPERATION = ("operation_id", "stage", "saved", "partial")
_STATE = ("current_activity", "activity_settling", "expected_boot_id", "expected_state_revision")


class Activity:
    """A created activity owned by an active TestBed and optionally a context."""

    def __init__(self, dut, name, *, display, source, audio=None, timeout=None):
        bed = getattr(dut, "_testbed", None)
        if bed is None or bed.session is None or not bed._entered or bed._closed:
            raise RuntimeError("Create activities inside an active TestBed")
        if (not isinstance(name, str) or not name or len(name.encode("utf-8")) > 63
                or any(ord(character) < 32 or ord(character) == 127 for character in name)):
            raise ValueError("An activity needs a name of at most 63 bytes")
        self.dut, self.name, self.session = dut, name, bed.session
        self.timeout = dut._runtime.timeout if timeout is None else timeout
        _positive_seconds(self.timeout, "Activity timeout")
        self.components = {"display": display, "source": source, "audio": audio}
        self.scan = dut.last_scan
        if self.scan is None or self.scan.dut is not dut or self.scan._session is not self.session:
            raise RuntimeError("Scan this DUT in the current TestBed before creating an activity")
        self.boot = self.scan._boot
        if not _identifier(self.boot):
            raise RuntimeError("The scan has no verified DUT boot identity")
        for component in self.components.values():
            if component is None:
                continue
            if component.dut is not dut or component._session is not self.session or component not in self.scan.expected:
                raise ValueError("Activity equipment must belong to this DUT's current scanned fixture")
            initiator = self.scan.initiators.get(component.gpio)
            if initiator is None or not _fresh_discovery(self.scan.records, component, initiator, self.scan.started):
                raise AssertionError("Activity equipment has no fresh physical discovery evidence")
        self.activity_id = None
        self._before_ids, self._before_equipment = set(), set()
        self._new_equipment = {}
        self._allocated_equipment = set()
        self._creation_request_id = None
        self._creation_operation = None
        self._creation_rejected = False
        self._last_start_request = None
        self._last_start_terminal = False
        self._write_attempted = self._created = self._closed = self._needs_standby = False
        self._row_removed = False
        self.retained_equipment = []
        bed.add_cleanup(self.close)

    def _record(self, phase, **values):
        record = {"phase": phase, "name": self.name, "activity_id": self.activity_id, **values}
        self.dut.activity_operations.append(record)
        return record

    def _remaining(self, deadline):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Activity operation deadline expired")
        return remaining

    def _view(self, *, check_health=True):
        if check_health:
            self.session.assert_healthy()
        document, state = self.dut.get_configuration(), self.dut.get_state()
        if state.get("expected_boot_id") != self.boot:
            raise AssertionError("DUT restarted during the activity test")
        if (document.get("version") != 1 or not isinstance(document.get("activities"), list)
                or document.get("expected_config_revision") != state.get("expected_config_revision")
                or document.get("expected_presentation_revision") != state.get("expected_presentation_revision")):
            raise ActivityConflict("Activity configuration changed while reading the UI")
        return document, state

    @staticmethod
    def _require_standby(state):
        if state.get("current_activity") != -1 or state.get("activity_settling", False):
            raise ActivityConflict("Activity configuration writes require the DUT to be in standby")

    def _full_configuration(self, document):
        full = self.dut.request("/api/v2/config")
        if (not isinstance(full, dict) or not isinstance(full.get("components"), list)
                or full.get("config_revision") != document["expected_config_revision"]
                or full.get("presentation_revision") != document["expected_presentation_revision"]):
            raise ActivityConflict("Activity configuration changed while reading its equipment")
        return full

    def _selectors(self, document, state):
        catalog = _ObservedEquipment(self.dut, state)
        resolved = [catalog.resolve(row) for row in state.get("devices", [])]
        selections = {}
        for role, component in self.components.items():
            if component is None:
                selections[role] = ""
                continue
            rows = [row for row, name, runtime in resolved
                    if _observed_matches((row, name, runtime), component, component.endpoint)]
            if len(rows) != 1:
                raise AssertionError("Activity equipment is missing or ambiguous in the current UI")
            selector = rows[0]["name"]
            choices = [choice for choice in document.get("device_choices", [])
                       if choice.get("value") == selector and role in choice.get("roles", [])]
            if len(choices) != 1:
                raise AssertionError("The scanned selection is unavailable for this activity role")
            selections[role] = selector
        return selections

    def _editor_payload(self, document, rows):
        return {"version": 1, "request_id": uuid4().hex,
                "expected_config_revision": document["expected_config_revision"],
                "expected_presentation_revision": document["expected_presentation_revision"],
                "activities": deepcopy(rows)}

    def _write(self, path, payload, deadline, trace, *, stage):
        try:
            response = self.dut.request(path, payload, timeout=min(15, self._remaining(deadline)))
        except AmityHTTPError:
            if trace["phase"] == "create":
                self._creation_rejected = True
            elif trace["phase"] == "start":
                self._last_start_terminal = True  # Explicit rejection admitted no own work.
            raise
        if trace["phase"] == "create":
            self._creation_operation = deepcopy(response)
        if response.get("operation_id") != payload["request_id"]:
            raise AssertionError("Amity returned a different activity operation identity")
        result = self.dut.wait_operation(response, timeout=self._remaining(deadline))
        trace["operation"] = {key: result[key] for key in _OPERATION if key in result}
        if result.get("operation_id") != payload["request_id"] or result.get("stage") != stage or result.get("partial"):
            raise AssertionError("The requested activity operation did not complete successfully")
        return result

    def _recognize(self, document, state):
        candidates = [row for row in document["activities"]
                      if row.get("activity_id") not in self._before_ids and row.get("name") == self.name]
        if not candidates:
            return False
        if len(candidates) != 1 or not _identifier(candidates[0].get("activity_id")):
            raise AssertionError("The created activity identity is ambiguous")
        row = candidates[0]
        if any(row.get(role, "") != selector for role, selector in self._selectors(document, state).items()):
            raise AssertionError("The created activity selectors differ from the scanned equipment")
        self.activity_id = row["activity_id"]
        self._created = True
        if any(not row.get(role, "").startswith("component:") or not _identifier(row[role][len("component:"):])
               for role, component in self.components.items() if component is not None):
            raise AssertionError("The created activity has no persisted scoped equipment identities")
        selected = {row.get(role, "").removeprefix("component:") for role in self.components if row.get(role)}
        self._allocated_equipment = selected - self._before_equipment
        full = self._full_configuration(document)
        for record in full["components"]:
            identity = record.get("component_id")
            if identity in selected and identity not in self._before_equipment:
                self._new_equipment[identity] = deepcopy(record)
        return True

    def create(self):
        if self._created:
            return self
        if self._write_attempted or self._closed:
            raise RuntimeError("An activity creation cannot be repeated")
        deadline = time.monotonic() + self.timeout
        trace = self._record("create")
        try:
            document, state = self._view()
            self._require_standby(state)
            if len(document["activities"]) >= 16 or any(row.get("name") == self.name for row in document["activities"]):
                raise ValueError("The activity list is full or this activity name already exists")
            full = self._full_configuration(document)
            self._before_ids = {row["activity_id"] for row in document["activities"]}
            self._before_equipment = {record["component_id"] for record in full["components"]}
            rows = [*document["activities"], {"name": self.name, **self._selectors(document, state)}]
            payload = self._editor_payload(document, rows)
            trace.update(request_id=payload["request_id"], expected_config_revision=payload["expected_config_revision"],
                         expected_presentation_revision=payload["expected_presentation_revision"])
            self._write_attempted = True
            self._creation_request_id = payload["request_id"]
            self._write("/api/v2/activity-editor", payload, deadline, trace, stage="applied")
            document, state = self._view()
            if not self._recognize(document, state):
                raise AssertionError("The created activity did not appear in the UI")
            trace["activity_id"] = self.activity_id
            return self
        except BaseException as error:
            trace["error_type"] = type(error).__name__
            raise

    def _action(self, kind, *, check_health=True):
        if self._closed or not self._created:
            raise RuntimeError("The test activity is not available")
        deadline = time.monotonic() + self.timeout
        document, state = self._view(check_health=check_health)
        matches = [index for index, row in enumerate(document["activities"]) if row.get("activity_id") == self.activity_id]
        if kind == "start":
            if len(matches) != 1:
                raise AssertionError("The created activity is missing or ambiguous")
            row = document["activities"][matches[0]]
            if any(row.get(role, "") != selector for role, selector in self._selectors(document, state).items()):
                raise AssertionError("The test activity equipment changed before start")
        payload = {"request_id": uuid4().hex, "activity_id": self.activity_id if kind == "start" else None,
                   "expected_config_revision": document["expected_config_revision"],
                   "expected_state_revision": state["expected_state_revision"], "expected_boot_id": self.boot}
        action = ActivityAction(self.dut, self.session, time.monotonic_ns(), self.boot,
                                deepcopy(self.scan.initiators), kind, payload["request_id"], payload["activity_id"])
        trace = self._record(kind, request_id=action.request_id, started_monotonic_ns=action.started_ns,
                             boot=self.boot, expected_config_revision=payload["expected_config_revision"])
        if kind == "start":
            self._needs_standby = True  # Admission may succeed even if its HTTP reply is lost.
            self._last_start_request, self._last_start_terminal = action.request_id, False
        try:
            action.operation = self._write("/api/v2/activity-editor/activity", payload, deadline, trace, stage="complete")
            if kind == "start":
                self._last_start_terminal = True

            def settled(current):
                if check_health:
                    self.session.assert_healthy()
                if current.get("expected_boot_id") != self.boot:
                    raise AssertionError("DUT restarted during the activity action")
                if (current.get("expected_config_revision") != document["expected_config_revision"]
                        or current.get("expected_presentation_revision") != document["expected_presentation_revision"]):
                    raise ActivityConflict("Activity configuration changed before the action settled")
                trace["state"] = {key: current[key] for key in _STATE if key in current}
                target = matches[0] if kind == "start" else -1
                return not current.get("activity_settling", False) and current.get("current_activity") == target

            action.state = self.dut.wait_for(settled, timeout=self._remaining(deadline))
            if kind == "standby":
                self._needs_standby = False
            return action
        except BaseException as error:
            trace["error_type"] = type(error).__name__
            raise

    def start(self):
        return self._action("start")

    def standby(self):
        return self._action("standby")

    def assert_active(self):
        """Assert this exact saved activity is active on the scanned DUT boot."""
        trace = self._record("assert_active")
        try:
            if self._closed or not self._created:
                raise RuntimeError("The test activity is not available")
            document, state = self._view()
            trace["state"] = {key: state[key] for key in _STATE if key in state}
            matches = [index for index, row in enumerate(document["activities"])
                       if row.get("activity_id") == self.activity_id]
            if len(matches) != 1:
                raise AssertionError("The created activity is missing or ambiguous")
            row = document["activities"][matches[0]]
            if any(row.get(role, "") != selector for role, selector in self._selectors(document, state).items()):
                raise AssertionError("The test activity equipment changed")
            current = state.get("current_activity")
            if type(current) is not int or current != matches[0]:
                raise AssertionError("The test activity is not the current activity")
            if state.get("activity_settling") is not False:
                raise AssertionError("The test activity has not finished settling")
            if state.get("activity_error") != "":
                raise AssertionError("The DUT has an activity execution error")
            return self
        except BaseException as error:
            trace["error_type"] = type(error).__name__
            raise

    def _remove_row(self, deadline):
        for attempt in range(3):
            try:
                document, state = self._view(check_health=False)
                if self.activity_id is None:
                    if not self._recognize(document, state):
                        return
                matches = [row for row in document["activities"] if row.get("activity_id") == self.activity_id]
                if not matches:
                    self._row_removed = True
                    return
                if len(matches) != 1:
                    raise AssertionError("The owned activity identity is duplicated")
                rows = [row for row in document["activities"] if row.get("activity_id") != self.activity_id]
                self._require_standby(state)
                trace = self._record("remove_activity")
                self._write("/api/v2/activity-editor", self._editor_payload(document, rows), deadline, trace, stage="applied")
                if any(row.get("activity_id") == self.activity_id for row in self._view(check_health=False)[0]["activities"]):
                    raise AssertionError("The created activity remains after cleanup")
                self._row_removed = True
                return
            except (ActivityConflict, AmityHTTPError) as error:
                if attempt == 2 or isinstance(error, AmityHTTPError) and error.status != 409:
                    raise

    def _recover_creation(self, deadline):
        if self.activity_id is not None or self._creation_rejected:
            return
        operation = self._creation_operation
        if operation is None:
            operation = self.dut.request("/api/v2/operations/" + self._creation_request_id)
        result = self.dut.wait_operation(operation, timeout=self._remaining(deadline))
        if result.get("operation_id") != self._creation_request_id or result.get("stage") != "applied":
            raise AssertionError("The partial activity creation outcome is unconfirmed")
        if not self._recognize(*self._view(check_health=False)):
            raise AssertionError("The partial activity creation could not be identified safely")

    def _prune_equipment(self, deadline):
        if not self._new_equipment:
            return sorted(self._allocated_equipment)
        for attempt in range(3):
            try:
                document, state = self._view(check_health=False)
                self._require_standby(state)
                full = self._full_configuration(document)
                references = {row.get(role) for row in full.get("activities", [])
                              for role in ("source", "display", "audio", "switch_device")}
                removable = {record["component_id"] for record in full["components"]
                             if record.get("component_id") in self._new_equipment
                             and record == self._new_equipment[record["component_id"]]
                             and record["component_id"] not in references}
                retained = [record["component_id"] for record in full["components"]
                            if record.get("component_id") in self._new_equipment and record["component_id"] not in removable]
                if removable:
                    candidate = deepcopy(full)
                    candidate["components"] = [record for record in candidate["components"] if record["component_id"] not in removable]
                    payload = {"request_id": uuid4().hex, "expected_config_revision": full["config_revision"],
                               "expected_presentation_revision": full["presentation_revision"], "configuration": candidate}
                    trace = self._record("remove_equipment", removed_equipment=sorted(removable))
                    self._write("/api/v2/config", payload, deadline, trace, stage="applied")
                    after = self._full_configuration(self._view(check_health=False)[0])
                    if any(record.get("component_id") in removable for record in after["components"]):
                        raise AssertionError("New fixture equipment remains after cleanup")
                return sorted(set(retained) | (self._allocated_equipment - set(self._new_equipment)))
            except (ActivityConflict, AmityHTTPError) as error:
                if attempt == 2 or isinstance(error, AmityHTTPError) and error.status != 409:
                    raise

    def _cleanup_standby(self):
        document, state = self._view(check_health=False)
        index = state.get("current_activity", -1)
        current = document["activities"][index].get("activity_id") if isinstance(index, int) and 0 <= index < len(document["activities"]) else None
        if current != self.activity_id:
            # A completed own request cannot authorize stopping an activity
            # subsequently selected by another user. Pending own work still
            # needs the ordinary standby cancellation before row removal.
            terminal = self._last_start_terminal
            if not terminal and self._last_start_request:
                operation = self.dut.request("/api/v2/operations/" + self._last_start_request)
                if operation.get("operation_id") != self._last_start_request:
                    raise AssertionError("Cleanup cannot identify the pending activity request")
                terminal = operation.get("stage") in {"complete", "failed", "rejected", "cancelled"}
            if terminal:
                self._needs_standby = False
                return
        self._action("standby", check_health=False)

    def close(self):
        if self._closed:
            return
        errors = []
        deadline = time.monotonic() + self.timeout
        self.retained_equipment = sorted(self._allocated_equipment)
        if self._needs_standby:
            try:
                self._cleanup_standby()
            except BaseException as error:
                errors.append(error)
        if self._write_attempted and not self._creation_rejected:
            try:
                self._recover_creation(deadline)
                self._remove_row(deadline)
                self.retained_equipment = self._prune_equipment(deadline)
            except BaseException as error:
                errors.append(error)
        trace = self._record("cleanup", removed_activity=self._row_removed or not self._write_attempted or self._creation_rejected,
                             retained_equipment=list(self.retained_equipment))
        if errors:
            trace["error_type"] = ",".join(type(error).__name__ for error in errors)
            raise ActivityCleanupError(errors)
        self._closed = True

    def __enter__(self):
        return self.create()

    def __exit__(self, exc_type, exc, traceback):
        self.close()
        return False
