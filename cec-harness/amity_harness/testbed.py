"""Generic functional-test resource ownership, authentication and evidence."""

from copy import deepcopy
import logging
import threading

from .components import Component
from .config import current_config, register_testbed
from .dut import DUT
from .topology import format_physical_address
from .trace import event, failure_fields


_ownership_lock = threading.RLock()
_owners = {}


class TestBedCleanupError(RuntimeError):
    """All cleanup was attempted, but one or more owned resources failed."""

    def __init__(self, errors):
        self.errors = tuple(errors)
        super().__init__("Test bed cleanup failed: " + ", ".join(type(error).__name__ for error in errors))


_DEVICE_FIELDS = frozenset(("name", "title", "location", "bus", "address",
                            "physical_address", "type", "vendor_id"))
_STATE_FIELDS = frozenset(("ready", "expected_boot_id", "discovery_generation", "discovering",
                           "front_address", "back_address", "tx_errors", "rx_dropped",
                           "front_physical_address", "determined_front_physical_address",
                           "current_activity", "activity_settling", "activity_error", "expected_state_revision"))
_TRAFFIC_FIELDS = frozenset(("timestamp", "direction", "component", "gpio", "frame", "source",
                             "destination", "opcode", "transport", "wire_timestamp_ns", "status",
                             "ok", "recipient", "recipients", "dropped", "handled_frame",
                             "handled_timestamp_ns"))
_OPERATION_FIELDS = frozenset(("operation_id", "stage", "status", "saved", "unresolved"))
_PORT_FIELDS = frozenset(("consortium_id", "octopus_id", "tentacle_id", "side"))
_POWER_FIELDS = frozenset(("command", "requested_power", "frame", "source", "destination", "transport",
                           "wire_timestamp_ns", "processed_monotonic_ns", "before", "after",
                           "state_changed", "state_revision", "payload_modified", "applied"))
_POWER_ASSERTION_FIELDS = frozenset(("kind", "power", "timestamp_ns", "since_monotonic_ns", "frame",
                                     "source", "destination", "wire_timestamp_ns", "command",
                                     "trigger_wire_timestamp_ns", "trigger_frame", "max_elapsed_ns", "response_latency_ns"))
_ROUTING_FIELDS = frozenset(("opcode", "physical_address", "input", "command", "frame", "source", "destination", "transport",
                            "wire_timestamp_ns", "processed_monotonic_ns", "state_revision",
                            "payload_modified", "applied"))
_ROUTING_ASSERTION_FIELDS = frozenset(("kind", "physical_address", "input", "command", "timestamp_ns", "since_monotonic_ns",
                                      "frame", "source", "destination", "wire_timestamp_ns",
                                      "trigger_wire_timestamp_ns", "trigger_frame", "max_elapsed_ns", "response_latency_ns",
                                      "observation_end_monotonic_ns", "observed_wires", "observed_frame_count", "idle_receive_since_ns"))
_SETTINGS_FIELDS = frozenset(("phase", "dut", "device_id", "tentacle_id", "request_id", "old_boot", "address",
                              "previous_address", "stage", "member_boot", "settings_revision", "error_type",
                              "configured"))
_ACTIVITY_FIELDS = frozenset(("phase", "name", "activity_id", "request_id", "started_monotonic_ns",
                              "boot", "expected_config_revision", "expected_presentation_revision", "error_type",
                              "removed_activity", "removed_equipment", "retained_equipment"))
_KEY_FIELDS = frozenset(("key", "emitted_key", "duration_ms", "started_monotonic_ns", "boot", "owner_boots", "released", "standby",
                        "deadline_monotonic_ns",
                        "component", "owner_id", "gpio", "press_frame", "release_frame", "presses",
                        "press_wire_timestamp_ns", "release_wire_timestamp_ns"))
_MEMBERSHIP_FIELDS = frozenset(("phase", "candidate", "device_id", "name", "request_id", "scan_id",
                               "operation_stage", "candidate_id", "boot", "member_boot",
                               "membership_revision", "reset", "retained_member", "error_type",
                               "step", "reset_gpio", "hold_seconds", "released", "inventory_removed"))
_MEMBERSHIP_REQUEST_FIELDS = frozenset(("method", "path", "started_ns", "finished_ns", "duration_ms",
                                       "admission_phase", "error_type", "http_status"))


def _select(value, fields):
    return {key: deepcopy(item) for key, item in value.items() if key in fields} if isinstance(value, dict) else {}


def _traffic(records):
    return [_select(record, _TRAFFIC_FIELDS) for record in records]


def _activity_report(dut, record):
    result = {"dut": dut.name, **_select(record, _ACTIVITY_FIELDS)}
    for key, fields in (("operation", _OPERATION_FIELDS | {"partial"}), ("state", _STATE_FIELDS)):
        if isinstance(record.get(key), dict):
            result[key] = _select(record[key], fields)
    return result


def _membership_report(dut, record):
    result = {"dut": dut.name, **_select(record, _MEMBERSHIP_FIELDS)}
    if isinstance(record.get("requests"), list):
        result["requests"] = [_select(request, _MEMBERSHIP_REQUEST_FIELDS) for request in record["requests"]]
    return result


def _scan_report(dut, scan):
    state = _select(getattr(scan, "status", {}), _STATE_FIELDS)
    devices = [_select(row, _DEVICE_FIELDS) for row in getattr(scan, "devices", [])]
    state["devices"] = [_select(row, _DEVICE_FIELDS)
                        for row in getattr(scan, "status", {}).get("devices", [])]
    return {"dut": dut.name, "started_monotonic_ns": getattr(scan, "started", 0),
            "owner_boots": deepcopy(getattr(scan, "owner_boots", {})),
            "owner_failures": deepcopy(getattr(scan, "owner_failures", ())),
            "operation": _select(getattr(scan, "operation", {}), _OPERATION_FIELDS),
            "devices": devices, "last_state": state,
            "dut_cec_initiators": deepcopy(getattr(scan, "initiators", {})),
            "locations": [{"component": location.get("component"),
                           "qualified_name": _select(location.get("qualified_name", {}), _PORT_FIELDS | {"osd_name"}),
                           "runtime_address": _select(location.get("runtime_address", {}), _PORT_FIELDS | {"logical_address"})}
                          for location in getattr(scan, "locations", [])],
            "cec": _traffic(getattr(scan, "records", ()))}


class TestBed:
    """Own DUTs and configurable component resources for one test scope.

    Accept DUTs and/or Components in any order; a component implies its DUT.
    Authentication and documented identity checks precede CEC adapter mutation.
    ``control_only`` DUTs are physical resources without HTTP or components.
    ``members`` DUTs can host CEC components without a member HTTP session;
    their owner identity is verified through the main's Add Amity flow.
    Each owned login is logged out after session cleanup, including failures.
    Foreign active DUTs are rejected before this bed acquires any resource.
    Reports retain only selected fixture/UI/CEC evidence, never runtime config,
    credentials, bearer tokens, HTTP headers or DUT URLs.
    """

    __test__ = False

    def __init__(self, *resources, session_factory=None, control_only=(), members=()):
        self.session = None
        self.duts = ()
        self.components = ()
        self._config = current_config()
        self._session_factory = session_factory if session_factory is not None else self._config.session_factory
        if not callable(self._session_factory):
            raise TypeError("session_factory must be callable")
        self._started = self._entered = self._finished = self._closed = False
        self._session_entered = False
        self._owned = []
        self._cleanups = []
        self._activities = []
        self._closing = False
        self._cleanup_errors = []
        self._failure = None
        self._failure_details = None
        self._initial_scans = {}
        self._initial_activity_counts = {}
        self._initial_key_counts = {}
        self._initial_membership_counts = {}
        self._initial_settings_counts = {}
        self._member_proofs = {}
        if not resources:
            raise ValueError("A test bed needs at least one DUT or component")
        explicit, duts, components = set(), [], []
        for resource in resources:
            if id(resource) in explicit:
                raise ValueError("A test bed resource was supplied more than once")
            explicit.add(id(resource))
            if isinstance(resource, DUT):
                dut = resource
            elif isinstance(resource, Component):
                components.append(resource)
                dut = resource.dut
                if not isinstance(dut, DUT):
                    raise TypeError("A component must reference a DUT")
            else:
                raise TypeError("TestBed accepts DUT and Component resources")
            if not any(existing is dut for existing in duts):
                duts.append(dut)
        names, identities = set(), set()
        for dut in duts:
            if dut.name in names or dut.identity.device_id in identities:
                raise ValueError("Two DUT objects refer to the same physical board")
            names.add(dut.name)
            identities.add(dut.identity.device_id)
        self.duts, self.components = tuple(duts), tuple(components)
        self.control_only = tuple(control_only)
        self.members = tuple(members)
        if any(not isinstance(dut, DUT) or not any(dut is member for member in self.duts)
               for dut in self.control_only):
            raise ValueError("Control-only DUTs must be resources owned by this TestBed")
        if len({id(dut) for dut in self.control_only}) != len(self.control_only):
            raise ValueError("A control-only DUT was supplied more than once")
        if any(not isinstance(dut, DUT) or not any(dut is member for member in self.duts)
               for dut in self.members):
            raise ValueError("Headless members must be resources owned by this TestBed")
        if len({id(dut) for dut in self.members}) != len(self.members):
            raise ValueError("A headless member was supplied more than once")
        if any(dut in self.control_only for dut in self.members):
            raise ValueError("A DUT cannot be both control-only and a headless member")
        if self.members and not any(dut not in (*self.members, *self.control_only) for dut in self.duts):
            raise ValueError("Headless members require a main DUT in the same TestBed")
        if any(component.dut in self.control_only for component in self.components):
            raise ValueError("Control-only DUTs cannot host CEC components in this TestBed")
        if any(getattr(dut, "_token", None) for dut in (*self.control_only, *self.members)):
            raise ValueError("Control-only DUTs and headless members must not have an authenticated HTTP session")
        register_testbed(self)
        event("testbed.created", level=logging.DEBUG, duts=[dut.name for dut in self.duts],
              components=[component.name for component in self.components])

    def _claim_duts(self):
        with _ownership_lock:
            for dut in self.duts:
                if getattr(dut, "_testbed", None) is not None or dut.identity.device_id in _owners:
                    raise RuntimeError(f"{dut.name} already belongs to an active test bed")
            for dut in self.duts:
                dut._testbed = self
                _owners[dut.identity.device_id] = self
                self._owned.append(dut)
                self._initial_scans[id(dut)] = getattr(dut, "last_scan", None)
                self._initial_activity_counts[id(dut)] = len(getattr(dut, "activity_operations", ()))
                self._initial_key_counts[id(dut)] = len(getattr(dut, "key_operations", ()))
                self._initial_membership_counts[id(dut)] = len(getattr(dut, "membership_operations", ()))
                self._initial_settings_counts[id(dut)] = len(getattr(dut, "adapter_settings_operations", ()))
                event("testbed.dut.claimed", dut=dut.name, device_id=dut.identity.device_id,
                      control_only=dut in self.control_only, member=dut in self.members)

    def __enter__(self):
        if self._started:
            raise RuntimeError("A test bed cannot be reused")
        self._started = True
        event("testbed.setup.start", duts=[dut.name for dut in self.duts],
              components=[component.name for component in self.components])
        try:
            self._claim_duts()
            for dut in self.duts:
                if dut in (*self.control_only, *self.members):
                    event("testbed.dut.physical_only", dut=dut.name,
                          member=dut in self.members, control_only=dut in self.control_only)
                    continue
                event("testbed.dut.authenticate", dut=dut.name)
                dut.authenticate()
                system = dut.get_system()
                if system.get("octopus_id") != dut.identity.device_id:
                    raise AssertionError(f"DUT HTTP identity does not match {dut.name} in TEST-HARNESS.md")
                event("testbed.dut.verified", dut=dut.name, device_id=dut.identity.device_id,
                      boot=system.get("boot_id"))
            event("testbed.cec.start", components=[component.name for component in self.components])
            self.session = self._session_factory(self.components)
            self.session.__enter__()
            self._session_entered = True
            self.session.assert_healthy()
            self._entered = True
            event("testbed.setup.complete", duts=[dut.name for dut in self.duts])
            return self
        except BaseException as exc:
            self._failure = type(exc).__name__
            self._failure_details = failure_fields(exc)
            event("testbed.setup.failed", level=logging.ERROR,
                  duts=[dut.name for dut in self.duts], error_type=self._failure)
            errors = self._cleanup(type(exc), exc, exc.__traceback__)
            if errors:
                raise TestBedCleanupError(errors) from exc
            raise

    def add_cleanup(self, callback):
        """Release test-owned UI resources before CEC responders and logout."""
        if not callable(callback):
            raise TypeError("A cleanup must be callable")
        if not self._entered or self._closed or self._closing:
            raise RuntimeError("Register cleanups inside an active TestBed")
        self._cleanups.append(callback)
        event("testbed.cleanup.registered", level=logging.DEBUG,
              callback=getattr(callback, "__qualname__", type(callback).__name__))

    def _cleanup(self, exc_type, exc, traceback):
        if self._closed or self._closing:
            return []
        self._closing = True
        errors = []
        event("testbed.cleanup.start", duts=[dut.name for dut in self._owned],
              callbacks=len(self._cleanups), failure=self._failure)
        try:
            callbacks = sorted(reversed(self._cleanups),
                               key=lambda callback: getattr(getattr(callback, "__self__", None), "_cleanup_priority", 1))
            self._cleanups.clear()
            for priority in sorted({getattr(getattr(callback, "__self__", None), "_cleanup_priority", 1)
                                    for callback in callbacks}):
                pending = [callback for callback in callbacks
                           if getattr(getattr(callback, "__self__", None), "_cleanup_priority", 1) == priority]
                completed_activities = []
                # Only the owned resource helpers support reconciliation. A
                # generic cleanup callback may have arbitrary effects and runs
                # once. Keep CEC responders and authentication alive throughout.
                for attempt in range(2):
                    retry = []
                    failures = []
                    for callback in pending:
                        owner = getattr(callback, "__self__", None)
                        label = getattr(callback, "__qualname__", type(callback).__name__)
                        event("testbed.cleanup.callback.start", callback=label, attempt=attempt + 1)
                        try:
                            callback()
                            if getattr(owner, "_cleanup_deferred", False):
                                raise RuntimeError("Owned cleanup is waiting for another fixture")
                        except BaseException as error:
                            if not attempt and getattr(owner, "_cleanup_retryable", False):
                                retry.append(callback)
                            else:
                                failures.append(error)
                            event("testbed.cleanup.callback.failed", level=logging.ERROR,
                                  callback=label, attempt=attempt + 1, error_type=type(error).__name__)
                        else:
                            if owner in self._activities and owner not in completed_activities:
                                completed_activities.append(owner)
                            event("testbed.cleanup.callback.complete", callback=label, attempt=attempt + 1)
                    errors.extend(failures)
                    if not retry:
                        break
                    pending = retry
                # A sibling may remove its row only on the recovery pass.
                # Revisit successful, closed activities before settings/member
                # cleanup so their exact-record pruning sees those new orphans.
                for activity in completed_activities:
                    if not activity._closed or not activity.retained_equipment:
                        continue
                    event("testbed.cleanup.activity_equipment.start", activity_id=activity.activity_id)
                    try:
                        activity.close()
                    except BaseException as error:
                        errors.append(error)
                        event("testbed.cleanup.activity_equipment.failed", level=logging.ERROR,
                              activity_id=activity.activity_id, error_type=type(error).__name__)
                    else:
                        event("testbed.cleanup.activity_equipment.complete", activity_id=activity.activity_id)
            if self.session is not None:
                event("testbed.cleanup.cec.start")
                try:
                    if self._session_entered:
                        self.session.__exit__(exc_type, exc, traceback)
                    elif callable(getattr(self.session, "close", None)):
                        self.session.close()
                except BaseException as error:
                    errors.append(error)
                    event("testbed.cleanup.cec.failed", level=logging.ERROR,
                          error_type=type(error).__name__)
                else:
                    event("testbed.cleanup.cec.complete")
            for dut in reversed(self._owned):
                if dut in (*self.control_only, *self.members):
                    continue
                if getattr(dut, "_token", None):
                    event("testbed.cleanup.logout.start", dut=dut.name)
                    try:
                        dut.logout()
                    except BaseException as error:
                        errors.append(error)
                        event("testbed.cleanup.logout.failed", level=logging.ERROR,
                              dut=dut.name, error_type=type(error).__name__)
                    else:
                        event("testbed.cleanup.logout.complete", dut=dut.name)
                try:
                    dut.close()
                except BaseException as error:
                    errors.append(error)
                    event("testbed.cleanup.http.failed", level=logging.ERROR,
                          dut=dut.name, error_type=type(error).__name__)
        finally:
            self._member_proofs.clear()
            self._closed = True
            self._closing = False
            with _ownership_lock:
                for dut in self._owned:
                    if getattr(dut, "_testbed", None) is self:
                        dut._testbed = None
                    if _owners.get(dut.identity.device_id) is self:
                        del _owners[dut.identity.device_id]
                    event("testbed.dut.released", dut=dut.name, device_id=dut.identity.device_id)
            self._cleanup_errors.extend(type(error).__name__ for error in errors)
            event("testbed.cleanup.complete", level=logging.ERROR if errors else logging.INFO,
                  duts=[dut.name for dut in self._owned],
                  error_types=[type(error).__name__ for error in errors])
        return errors

    def __exit__(self, exc_type, exc, traceback):
        health_error = None
        if exc is None:
            try:
                self.session.assert_healthy()
            except BaseException as error:
                health_error = error
                exc_type, exc, traceback = type(error), error, error.__traceback__
        if exc is not None:
            self._failure = type(exc).__name__
            self._failure_details = failure_fields(exc)
            event("testbed.failed", level=logging.ERROR, duts=[dut.name for dut in self.duts],
                  error_type=self._failure)
        errors = self._cleanup(exc_type, exc, traceback)
        self._finished = True
        event("testbed.complete", level=logging.ERROR if self._failure or errors else logging.INFO,
              duts=[dut.name for dut in self.duts], passed=not self._failure and not errors)
        if errors:
            raise TestBedCleanupError(errors) from exc
        if health_error is not None:
            raise health_error
        return False

    @property
    def report(self):
        """Credential-free evidence snapshot, available after partial failures."""
        components = []
        for component in self.components:
            row = {"name": component.name, "dut": component.dut.name,
                   "type": component.device_type, "gpio": component.gpio,
                   "tentacle": component.tentacle, "direction": component.direction}
            if component.parent is None:
                row["attachment"] = {"dut": component.dut.name,
                                     "tentacle": component.tentacle,
                                     "side": component.direction,
                                     "tv_input": component.tv_input}
            else:
                row["attachment"] = {"component": component.parent.name,
                                     "input": component.port}
            if component.address is not None:
                row["logical_address"] = component.address.logical
                row["physical_address"] = format_physical_address(component.address.physical)
            row["power"] = component.power.value
            row["cec_version"] = component.cec_version
            if hasattr(component, "address_type"):
                row["address_type"] = component.address_type
            row["power_events"] = [_select(event, _POWER_FIELDS) for event in component.power_events]
            row["power_assertions"] = [_select(event, _POWER_ASSERTION_FIELDS) for event in component.power_assertions]
            if component.can_route:
                row["active_input"] = component.active_input
                row["selected_input"] = component.selected_input
                row["routing_events"] = [_select(event, _ROUTING_FIELDS) for event in component.routing_events]
                row["routing_assertions"] = [_select(event, _ROUTING_ASSERTION_FIELDS) for event in component.routing_assertions]
            if hasattr(component, "active"):
                row["active"] = component.active
            components.append(row)
        scans = []
        for dut in self._owned:
            scan = getattr(dut, "last_scan", None)
            if scan is not None and scan is not self._initial_scans.get(id(dut)):
                scans.append(_scan_report(dut, scan))
        result = {"passed": self._finished and self._failure is None and not self._cleanup_errors,
                  "entered": self._entered,
                  "duts": [{"name": dut.name, "device_id": dut.identity.device_id,
                            "control_only": dut in self.control_only,
                            **({"member": dut in self.members} if self.members else {})} for dut in self.duts],
                  "components": components, "scans": scans,
                  "activities": [_activity_report(dut, record) for dut in self._owned
                                 for record in getattr(dut, "activity_operations", ())[self._initial_activity_counts[id(dut)]:]],
                  "keys": [{"dut": dut.name, **_select(record, _KEY_FIELDS)} for dut in self._owned
                           for record in getattr(dut, "key_operations", ())[self._initial_key_counts[id(dut)]:]],
                  "memberships": [_membership_report(dut, record) for dut in self._owned
                                  for record in getattr(dut, "membership_operations", ())[self._initial_membership_counts[id(dut)]:]],
                  "adapter_settings": [_select(record, _SETTINGS_FIELDS) for dut in self._owned
                                       for record in getattr(dut, "adapter_settings_operations", ())[self._initial_settings_counts[id(dut)]:]],
                  "cec": _traffic(self.session.records if self.session is not None else ()),
                  "cleanup_errors": list(self._cleanup_errors)}
        if self._failure is not None:
            result["error"] = self._failure
            result["failure"] = deepcopy(self._failure_details)
        return result
