"""Temporary activities through the existing authenticated activity-editor UI.

No firmware test API or restore operation is used. Cleanup merges against the
latest configuration, removes only this activity, and prunes only unchanged,
unreferenced equipment records allocated while creating its fixture selections.
"""

from copy import deepcopy
from dataclasses import dataclass, field
import logging
import time
from uuid import uuid4

from .discovery import _ObservedEquipment, _fresh_discovery, _identifier, _observed_matches
from .dut import AmityHTTPError, _positive_seconds
from .trace import event


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
    scan: object = field(default=None, repr=False)
    testbed: object = field(default=None, repr=False)
    owner_boots: dict = field(default_factory=dict)
    owner_duts: dict = field(default_factory=dict, repr=False)

    @property
    def started(self):
        return self.started_ns


_OPERATION = ("operation_id", "stage", "saved", "partial")
_STATE = ("current_activity", "activity_settling", "expected_boot_id", "expected_state_revision")


class Activity:
    """A created activity owned by an active TestBed and optionally a context."""

    _cleanup_priority = 0
    _cleanup_retryable = True

    def __init__(self, dut, name, *, display, source, audio=None,
                 switch_device=None, switch_input=0, add_components=(), timeout=None):
        bed = getattr(dut, "_testbed", None)
        if bed is None or bed.session is None or not bed._entered or bed._closed:
            raise RuntimeError("Create activities inside an active TestBed")
        if (not isinstance(name, str) or not name or len(name.encode("utf-8")) > 63
                or any(ord(character) < 32 or ord(character) == 127 for character in name)):
            raise ValueError("An activity needs a name of at most 63 bytes")
        if type(switch_input) is not int or not 0 <= switch_input <= 255:
            raise ValueError("Switch input must be an integer between 0 and 255")
        if switch_device is None and switch_input != 0:
            raise ValueError("A switch input requires a switch device")
        self.dut, self.name, self.session, self._testbed = dut, name, bed.session, bed
        self.timeout = dut._runtime.timeout if timeout is None else timeout
        _positive_seconds(self.timeout, "Activity timeout")
        self.components = {"display": display, "source": source, "audio": audio, "switch": switch_device}
        self._manual_components = tuple(add_components)
        if (len({id(component) for component in self._manual_components}) != len(self._manual_components)
                or any(component is None or component not in self.components.values()
                       for component in self._manual_components)):
            raise ValueError("Added components must be distinct selections of this activity")
        self.switch_input = switch_input
        self.scan = dut.last_scan
        if self.scan is None or self.scan.dut is not dut or self.scan._session is not self.session:
            raise RuntimeError("Scan this DUT in the current TestBed before creating an activity")
        self.boot = self.scan._boot
        if not _identifier(self.boot):
            raise RuntimeError("The scan has no verified DUT boot identity")
        for component in self.components.values():
            if component is None:
                continue
            if (component._session is not self.session
                    or getattr(component.dut, "_testbed", None) is not bed):
                raise ValueError("Activity equipment must belong to this TestBed's current scanned fixture")
            if component.dut is not dut and not any(component.dut is owner for owner in getattr(bed, "duts", ())):
                raise ValueError("Activity equipment belongs to an unrelated owner")
            witness = self._discovery_witness(component)
            initiator = self.scan.initiators.get(witness.gpio)
            if initiator is None or not _fresh_discovery(self.scan.records, witness, initiator, self.scan.started):
                raise AssertionError("Activity equipment has no fresh physical discovery evidence")
        self._validate_owners()
        self._action_scan = self.scan
        self.activity_id = None
        self._before_ids, self._before_equipment = set(), set()
        self._new_equipment = {}
        self._component_writes = []
        self._allocated_equipment = set()
        self._creation_request_id = None
        self._creation_operation = None
        self._creation_rejected = False
        self._last_start_request = None
        self._last_start_terminal = False
        self._write_attempted = self._created = self._closed = self._needs_standby = False
        self._row_removed = False
        self._created_row = None
        self._saved_group = None
        self._cleanup_trace = None
        self._cleanup_deferred = False
        self.retained_equipment = []
        bed.add_cleanup(self.close)
        if not hasattr(bed, "_activities"):
            bed._activities = []
        bed._activities.append(self)

    def _record(self, phase, **values):
        record = {"phase": phase, "name": self.name, "activity_id": self.activity_id, **values}
        self.dut.activity_operations.append(record)
        self._log("activity.phase", phase=phase, request_id=values.get("request_id"))
        return record

    def _log(self, name, *, level=logging.INFO, **fields):
        event(name, level=level, dut=self.dut.name, device_id=self.dut.identity.device_id,
              boot=self.boot, activity=self.name, activity_id=self.activity_id, **fields)

    def _remaining(self, deadline):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Activity operation deadline expired")
        return remaining

    def _validate_owners(self):
        equipment = tuple(component for component in self.components.values() if component is not None)
        if not any(component.dut is not self.dut for component in equipment):
            return
        if (getattr(self.scan, "_testbed", None) is not self._testbed
                or self._testbed.session is not self.session or self._testbed._closed
                or not self._testbed._entered or getattr(self.dut, "_testbed", None) is not self._testbed):
            raise AssertionError("Activity owner evidence belongs to a different TestBed")
        validator = getattr(self.scan, "validate_owners", None)
        if not callable(validator):
            raise AssertionError("Activity equipment has no verified owner boot evidence")
        validator(tuple(self._discovery_witness(component) for component in equipment))

    def _discovery_witness(self, component):
        if component in self.scan.expected:
            return component
        if component not in self._manual_components or component not in self.session.components:
            raise ValueError("Activity equipment must belong to this TestBed's current scanned fixture")
        owner = component.dut.identity.device_id
        if (self.scan.owner_duts.get(owner) is not component.dut
                or not _identifier(self.scan.owner_boots.get(owner))):
            raise AssertionError("Added equipment has no proven physical owner")
        witnesses = [item for item in self.scan.expected
                     if item.dut is component.dut and item.gpio == component.gpio]
        if not witnesses:
            raise AssertionError("Added equipment needs a scanned witness on its own CEC wire")
        return witnesses[0]

    def refresh_scan(self, scan=None):
        """Use fresh scan evidence for future actions of this saved activity.

        Keep its creation evidence and saved identity intact. Every selected
        component must be physically rediscovered on the same owner boot.
        """
        if self._closed or not self._created:
            raise RuntimeError("The test activity is not available")
        scan = self.dut.last_scan if scan is None else scan
        bed = self._testbed
        if (scan is None or scan is not self.dut.last_scan or scan.dut is not self.dut
                or scan._session is not self.session or getattr(scan, "_testbed", None) is not bed
                or bed.session is not self.session or not bed._entered or bed._closed
                or self.dut._testbed is not bed or scan._boot != self.boot
                or scan._group != self._saved_group or not scan.started):
            raise AssertionError("Replacement action evidence belongs to another current scan, boot or TestBed")
        equipment = tuple(component for component in self.components.values() if component is not None)
        for component in equipment:
            owner = component.dut.identity.device_id
            original_boot = (self.boot if component.dut is self.dut
                             else self.scan.owner_boots.get(owner))
            if (component not in scan.expected or component._session is not self.session
                    or component.dut._testbed is not bed
                    or scan.owner_duts.get(owner) is not component.dut
                    or not _identifier(original_boot) or scan.owner_boots.get(owner) != original_boot):
                raise AssertionError("Replacement action evidence changed a selected component's owner or boot")
            initiator = scan.initiators.get(component.gpio)
            if initiator is None or not _fresh_discovery(scan.records, component, initiator, scan.started):
                raise AssertionError("Replacement action evidence needs fresh physical discovery of every selection")
        scan.validate_owners(equipment)
        if self.dut.last_scan is not scan:
            raise AssertionError("The current scan changed while refreshing activity evidence")
        self._action_scan = scan
        return self

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
        full = self.dut.request("/api/config")
        if (not isinstance(full, dict) or not isinstance(full.get("components"), list)
                or full.get("config_revision") != document["expected_config_revision"]
                or full.get("presentation_revision") != document["expected_presentation_revision"]):
            raise ActivityConflict("Activity configuration changed while reading its equipment")
        return full

    def _selectors(self, document, state):
        self._validate_owners()
        catalog = _ObservedEquipment(self.dut, state)
        resolved = [catalog.resolve(row) for row in state.get("devices", [])]
        selections = {}
        for role, component in self.components.items():
            if component is None:
                selections[role] = ""
                continue
            if component in self._manual_components:
                full = self._full_configuration(document)
                records = [record for record in full["components"]
                           if record.get("qualified_names") == [self._qualified_name(component)]]
                if len(records) != 1 or not _identifier(records[0].get("component_id")):
                    raise AssertionError("Added equipment has no unique saved scoped identity")
                selector = "component:" + records[0]["component_id"]
                choices = [choice for choice in document.get("device_choices", [])
                           if choice.get("value") == selector and role in choice.get("roles", [])]
                if len(choices) != 1:
                    raise AssertionError("The added selection is unavailable for this activity role")
                selections[role] = selector
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

    def _qualified_name(self, component):
        return {"consortium_id": self._saved_group, "octopus_id": component.dut.identity.device_id,
                "tentacle_id": component.tentacle, "side": component.direction, "osd_name": component.name}

    def _recognize_component(self, write, result):
        if (result.get("operation_id") != write["request_id"] or result.get("stage") != "applied"
                or not _identifier(result.get("component_id"))):
            raise AssertionError("The Add component operation did not persist a component identity")
        document, _ = self._view(check_health=False)
        full = self._full_configuration(document)
        identity = result["component_id"]
        records = [record for record in full["components"] if record.get("component_id") == identity]
        if len(records) != 1 or records[0].get("qualified_names") != [self._qualified_name(write["component"])]:
            raise AssertionError("The added component differs from its requested scoped OSD name")
        if identity not in self._before_equipment:
            self._allocated_equipment.add(identity)
            self._new_equipment[identity] = deepcopy(records[0])
        write["complete"] = True
        write["trace"].update(component_id=identity, operation={key: result[key] for key in _OPERATION if key in result})

    def _add_components(self, document, deadline):
        for component in self._manual_components:
            role = next(role for role, selected in self.components.items() if selected is component)
            payload = {"request_id": uuid4().hex,
                       "expected_config_revision": document["expected_config_revision"],
                       "expected_presentation_revision": document["expected_presentation_revision"],
                       **{key: value for key, value in self._qualified_name(component).items() if key != "consortium_id"},
                       "role": role}
            trace = self._record("add_component", request_id=payload["request_id"],
                                 owner=component.dut.identity.device_id, tentacle=component.tentacle,
                                 side=component.direction, osd_name=component.name, role=role)
            write = {"request_id": payload["request_id"], "component": component,
                     "trace": trace, "complete": False, "rejected": False}
            self._component_writes.append(write)
            try:
                result = self._write("/api/activity-editor/components", payload, deadline, trace, stage="applied")
            except AmityHTTPError:
                write["rejected"] = trace.get("rejected", False)
                raise
            self._recognize_component(write, result)
            document, state = self._view()
            self._require_standby(state)
        return document

    def _recover_components(self, deadline):
        for write in self._component_writes:
            if not write["complete"] and not write["rejected"]:
                operation = self.dut.request("/api/operations/" + write["request_id"],
                                             timeout=min(15, self._remaining(deadline)))
                result = self.dut.wait_operation(operation, timeout=self._remaining(deadline))
                self._recognize_component(write, result)

    def _editor_payload(self, document, rows):
        return {"version": 1, "request_id": uuid4().hex,
                "expected_config_revision": document["expected_config_revision"],
                "expected_presentation_revision": document["expected_presentation_revision"],
                "activities": deepcopy(rows)}

    @staticmethod
    def _row_selectors(row):
        switch = row.get("switch")
        return {**{role: row.get(role, "") for role in ("display", "source", "audio")},
                "switch": switch.get("device", "") if isinstance(switch, dict) else ""}

    def _matches_selections(self, row, selections):
        if self._row_selectors(row) != selections:
            return False
        switch = row.get("switch")
        if self.components["switch"] is None:
            return switch is None
        return (isinstance(switch, dict) and set(switch) == {"device", "input"}
                and type(switch["input"]) is int and switch["input"] == self.switch_input)

    def _selection_fields(self, document, state):
        selections = self._selectors(document, state)
        switch = selections.pop("switch")
        if switch:
            selections["switch"] = {"device": switch, "input": self.switch_input}
        return selections

    def _saved_selections_match(self, document):
        """Validate saved intent without requiring a current CEC observation.

        Creation proves discovered selections or explicit Add component records.
        Later starts must also work while that equipment is absent. Keep both
        the activity row and its saved qualified equipment records fenced.
        """
        self._validate_owners()
        rows = [row for row in document["activities"] if row.get("activity_id") == self.activity_id]
        if len(rows) != 1 or rows[0] != self._created_row:
            return False
        full = self._full_configuration(document)
        selectors = self._row_selectors(rows[0])
        group = self._saved_group
        if not group or full.get("consortium_id") != group:
            return False
        for role, component in self.components.items():
            if component is None:
                continue
            identity = selectors[role].removeprefix("component:")
            records = [record for record in full["components"] if record.get("component_id") == identity]
            expected = {"consortium_id": group, "octopus_id": component.dut.identity.device_id,
                        "tentacle_id": component.tentacle, "side": component.direction,
                        "osd_name": component.name}
            if len(records) != 1 or records[0].get("qualified_names") != [expected]:
                return False
        return True

    def _write(self, path, payload, deadline, trace, *, stage):
        try:
            response = self.dut.request(path, payload, timeout=min(15, self._remaining(deadline)))
        except AmityHTTPError:
            if trace["phase"] == "create":
                self._creation_rejected = True
            elif trace["phase"] == "start":
                self._last_start_terminal = True  # Explicit rejection admitted no own work.
            elif trace["phase"] == "add_component":
                trace["rejected"] = True
            raise
        if trace["phase"] == "create":
            self._creation_operation = deepcopy(response)
        if response.get("operation_id") != payload["request_id"]:
            raise AssertionError("Amity returned a different activity operation identity")
        result = self.dut.wait_operation(response, timeout=self._remaining(deadline))
        trace["operation"] = {key: result[key] for key in _OPERATION if key in result}
        self._log("activity.operation", phase=trace["phase"], request_id=payload["request_id"],
                  operation_id=result.get("operation_id"), stage=result.get("stage"),
                  saved=result.get("saved"), partial=result.get("partial"))
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
        if not self._matches_selections(row, self._selectors(document, state)):
            raise AssertionError("The created activity selectors differ from the scanned equipment")
        self.activity_id = row["activity_id"]
        self._created = True
        self._created_row = deepcopy(row)
        selectors = self._row_selectors(row)
        if any(not selectors[role].startswith("component:") or not _identifier(selectors[role][len("component:"):])
               for role, component in self.components.items() if component is not None):
            raise AssertionError("The created activity has no persisted scoped equipment identities")
        selected = {selector.removeprefix("component:") for selector in selectors.values() if selector}
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
            self._saved_group = full.get("consortium_id")
            if (not _identifier(self._saved_group)
                    or self.scan._group and self._saved_group != self.scan._group):
                raise AssertionError("Saved activity configuration belongs to another system")
            self._before_ids = {row["activity_id"] for row in document["activities"]}
            self._before_equipment = {record["component_id"] for record in full["components"]}
            document = self._add_components(document, deadline)
            document, state = self._view()
            rows = [*document["activities"], {"name": self.name, **self._selection_fields(document, state)}]
            payload = self._editor_payload(document, rows)
            trace.update(request_id=payload["request_id"], expected_config_revision=payload["expected_config_revision"],
                         expected_presentation_revision=payload["expected_presentation_revision"])
            self._write_attempted = True
            self._creation_request_id = payload["request_id"]
            self._log("activity.create.submit", request_id=self._creation_request_id)
            self._write("/api/activity-editor", payload, deadline, trace, stage="applied")
            document, state = self._view()
            if not self._recognize(document, state):
                raise AssertionError("The created activity did not appear in the UI")
            trace["activity_id"] = self.activity_id
            self._log("activity.create.complete", request_id=self._creation_request_id)
            return self
        except BaseException as error:
            trace["error_type"] = type(error).__name__
            self._log("activity.create.failed", level=logging.ERROR,
                      request_id=self._creation_request_id, error_type=type(error).__name__)
            raise

    def _action(self, kind, *, check_health=True):
        if self._closed or not self._created:
            raise RuntimeError("The test activity is not available")
        if check_health:
            self._validate_owners()
        deadline = time.monotonic() + self.timeout
        document, state = self._view(check_health=check_health)
        matches = [index for index, row in enumerate(document["activities"]) if row.get("activity_id") == self.activity_id]
        if kind == "start":
            if len(matches) != 1:
                raise AssertionError("The created activity is missing or ambiguous")
            if not self._saved_selections_match(document):
                raise AssertionError("The test activity equipment changed before start")
        payload = {"request_id": uuid4().hex, "activity_id": self.activity_id if kind == "start" else None,
                   "expected_config_revision": document["expected_config_revision"],
                   "expected_state_revision": state["expected_state_revision"], "expected_boot_id": self.boot}
        scan = self._action_scan
        action = ActivityAction(self.dut, self.session, time.monotonic_ns(), self.boot,
                                deepcopy(scan.initiators), kind, payload["request_id"], payload["activity_id"],
                                scan=scan, testbed=self._testbed,
                                owner_boots=dict(getattr(scan, "owner_boots", {})),
                                owner_duts=dict(getattr(scan, "owner_duts", {})))
        trace = self._record(kind, request_id=action.request_id, started_monotonic_ns=action.started_ns,
                             boot=self.boot, expected_config_revision=payload["expected_config_revision"])
        if kind == "start":
            self._needs_standby = True  # Admission may succeed even if its HTTP reply is lost.
            self._last_start_request, self._last_start_terminal = action.request_id, False
        try:
            action.operation = self._write("/api/activity-editor/activity", payload, deadline, trace, stage="complete")
            if kind == "start":
                self._last_start_terminal = True
            previous_state = None

            def settled(current):
                nonlocal previous_state
                if check_health:
                    self.session.assert_healthy()
                if current.get("expected_boot_id") != self.boot:
                    raise AssertionError("DUT restarted during the activity action")
                if (current.get("expected_config_revision") != document["expected_config_revision"]
                        or current.get("expected_presentation_revision") != document["expected_presentation_revision"]):
                    raise ActivityConflict("Activity configuration changed before the action settled")
                trace["state"] = {key: current[key] for key in _STATE if key in current}
                observed = (current.get("current_activity"), current.get("activity_settling"))
                if observed != previous_state:
                    previous_state = observed
                    self._log("activity.settling", phase=kind, request_id=action.request_id,
                              current_activity=observed[0], settling=observed[1])
                target = matches[0] if kind == "start" else -1
                return not current.get("activity_settling", False) and current.get("current_activity") == target

            action.state = self.dut.wait_for(settled, timeout=self._remaining(deadline))
            if check_health:
                self._validate_owners()
            if kind == "standby":
                self._needs_standby = False
            self._log("activity.action.complete", phase=kind, request_id=action.request_id,
                      current_activity=action.state.get("current_activity"))
            return action
        except BaseException as error:
            trace["error_type"] = type(error).__name__
            self._log("activity.action.failed", level=logging.ERROR, phase=kind,
                      request_id=action.request_id, error_type=type(error).__name__)
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
            if not self._saved_selections_match(document):
                raise AssertionError("The test activity equipment changed")
            current = state.get("current_activity")
            if type(current) is not int or current != matches[0]:
                raise AssertionError("The test activity is not the current activity")
            if state.get("activity_settling") is not False:
                raise AssertionError("The test activity has not finished settling")
            if state.get("activity_error") != "":
                raise AssertionError("The DUT has an activity execution error")
            self._log("activity.assert_active.passed", current_activity=current)
            return self
        except BaseException as error:
            trace["error_type"] = type(error).__name__
            self._log("activity.assert_active.failed", level=logging.ERROR,
                      error_type=type(error).__name__)
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
                self._write("/api/activity-editor", self._editor_payload(document, rows), deadline, trace, stage="applied")
                if any(row.get("activity_id") == self.activity_id for row in self._view(check_health=False)[0]["activities"]):
                    raise AssertionError("The created activity remains after cleanup")
                self._row_removed = True
                self._log("activity.remove.complete")
                return
            except (ActivityConflict, AmityHTTPError) as error:
                self._log("activity.remove.conflict", level=logging.WARNING,
                          attempt=attempt + 1, error_type=type(error).__name__)
                if attempt == 2 or isinstance(error, AmityHTTPError) and error.status != 409:
                    raise

    def _recover_creation(self, deadline):
        if self.activity_id is not None or self._creation_rejected:
            return
        operation = self._creation_operation
        if operation is None:
            operation = self.dut.request("/api/operations/" + self._creation_request_id)
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
                    self._write("/api/config", payload, deadline, trace, stage="applied")
                    after = self._full_configuration(self._view(check_health=False)[0])
                    if any(record.get("component_id") in removable for record in after["components"]):
                        raise AssertionError("New fixture equipment remains after cleanup")
                    self._log("activity.equipment.removed", component_ids=sorted(removable))
                return sorted(set(retained) | (self._allocated_equipment - set(self._new_equipment)))
            except (ActivityConflict, AmityHTTPError) as error:
                self._log("activity.equipment.conflict", level=logging.WARNING,
                          attempt=attempt + 1, error_type=type(error).__name__)
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
                operation = self.dut.request("/api/operations/" + self._last_start_request)
                if operation.get("operation_id") != self._last_start_request:
                    raise AssertionError("Cleanup cannot identify the pending activity request")
                terminal = operation.get("stage") in {"complete", "failed", "rejected", "cancelled"}
            if terminal:
                self._needs_standby = False
                return
        self._action("standby", check_health=False)

    def close(self):
        if self._closed:
            # A sibling fixture may still have referenced equipment when this
            # activity's own row was removed. Its registered bed callback runs
            # after deferred rows have closed and must finish that same exact
            # ownership-limited pruning work.
            if self.retained_equipment and self._new_equipment:
                try:
                    self.retained_equipment = self._prune_equipment(time.monotonic() + self.timeout)
                except BaseException as error:
                    if self._cleanup_trace is not None:
                        self._cleanup_trace["error_type"] = type(error).__name__
                    raise ActivityCleanupError([error]) from error
                if self._cleanup_trace is not None:
                    self._cleanup_trace["retained_equipment"] = list(self.retained_equipment)
                    self._cleanup_trace.pop("error_type", None)
                self._log("activity.cleanup.equipment_rechecked", retained_equipment=list(self.retained_equipment))
            return
        self._cleanup_deferred = False
        # An unused fixture may close before the selected fixture's context.
        # Leave its registered cleanup for the bed instead of trying to edit
        # configuration while that other activity is still running. Ownership
        # includes the exact created row; an unrelated or edited activity never
        # authorizes this dependency handling.
        if self._created and (not self._needs_standby or self._last_start_terminal):
            document, state = self._view(check_health=False)
            index = state.get("current_activity", -1)
            current = document["activities"][index] if type(index) is int and 0 <= index < len(document["activities"]) else None
            if current is not None and current.get("activity_id") != self.activity_id:
                selected = next((activity for activity in getattr(self._testbed, "_activities", ())
                                 if activity is not self and activity.dut is self.dut
                                 and activity.boot == self.boot and activity._created and not activity._closed
                                 and activity._created_row == current and activity._needs_standby), None)
                if selected is not None:
                    self._cleanup_deferred = True
                    self._log("activity.cleanup.waiting_for_fixture", selected_activity_id=selected.activity_id)
                    return
        self._log("activity.cleanup.start")
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
            except BaseException as error:
                errors.append(error)
        try:
            self._recover_components(deadline)
            self.retained_equipment = self._prune_equipment(deadline)
        except BaseException as error:
            errors.append(error)
        trace = self._record("cleanup", removed_activity=self._row_removed or not self._write_attempted or self._creation_rejected,
                             retained_equipment=list(self.retained_equipment))
        self._cleanup_trace = trace
        if errors:
            trace["error_type"] = ",".join(type(error).__name__ for error in errors)
            self._log("activity.cleanup.failed", level=logging.ERROR,
                      error_types=[type(error).__name__ for error in errors],
                      removed_activity=trace["removed_activity"],
                      retained_equipment=list(self.retained_equipment))
            raise ActivityCleanupError(errors)
        self._closed = True
        self._log("activity.cleanup.complete", removed_activity=trace["removed_activity"],
                  retained_equipment=list(self.retained_equipment))

    def __enter__(self):
        return self.create()

    def __exit__(self, exc_type, exc, traceback):
        try:
            self.close()
        except Exception as cleanup_error:
            if exc is None:
                raise
            self._log("activity.cleanup.deferred", level=logging.ERROR,
                      error_type=type(cleanup_error).__name__, primary_error_type=type(exc).__name__)
            note = getattr(exc, "add_note", None)
            if callable(note):
                note("Activity cleanup failed; the registered TestBed cleanup will retry it.")
        return False
