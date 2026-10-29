"""Generic HDMI discovery through Amity's existing authenticated UI controls.

The framework adds no firmware test API. Scan assertions require the ordinary
UI result and fresh, physically observed identity exchanges on the DUT's wires;
virtual component traffic cannot establish a successful physical scan.
"""

from copy import deepcopy
from dataclasses import dataclass, field
import hashlib
import json
import math
import re
import time

from .connections import TentacleEndpoint


_PORT_KEYS = ("consortium_id", "octopus_id", "tentacle_id", "side")


def _identifier(value):
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{32}", value) is not None and value != "0" * 32


def _integer(value, low, high):
    return isinstance(value, int) and not isinstance(value, bool) and low <= value <= high


def _qualified(value, *, runtime=False):
    """Validate the existing firmware's flat typed identity representation."""
    extra = "logical_address" if runtime else "osd_name"
    if not isinstance(value, dict) or set(value) != set(_PORT_KEYS) | {extra}:
        raise AssertionError("Missing or malformed observed equipment identity")
    if (not _identifier(value["consortium_id"]) or not _identifier(value["octopus_id"])
            or not _integer(value["tentacle_id"], 0, 3) or value["side"] not in ("front", "back")):
        raise AssertionError("Malformed observed equipment port")
    if runtime:
        valid = _integer(value[extra], 0, 14)
    else:
        name = value[extra]
        try:
            valid = (isinstance(name, str) and 0 < len(name.encode("utf-8")) <= 14
                     and all(ord(character) >= 32 and ord(character) != 127 for character in name))
        except UnicodeError:
            valid = False
    if not valid:
        raise AssertionError("Malformed observed equipment address or OSD name")
    return {key: value[key] for key in (*_PORT_KEYS, extra)}


def _native_selector(qualified):
    # model.cpp::name_json uses these exact keys/order, compact formatting and
    # literal UTF-8. Valid OSD names contain no JSON control characters.
    encoded = json.dumps(_qualified(qualified), ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    return "native:" + hashlib.sha256(encoded).hexdigest()


class _ObservedEquipment:
    """Read existing UI data and correlate display rows to typed observations."""

    def __init__(self, dut, status):
        self.dut = dut
        self.status = status
        self.system = dut.get_system()
        if (not isinstance(self.system, dict)
                or self.system.get("octopus_id") != dut.identity.device_id
                or not _identifier(self.system.get("consortium_id"))
                or not isinstance(self.system.get("observations"), list)):
            raise AssertionError("System snapshot has no valid DUT equipment identities")
        if (not _identifier(status.get("expected_boot_id"))
                or self.system.get("boot_id") != status["expected_boot_id"]):
            raise AssertionError("DUT boot changed before the equipment location assertion")
        self.configuration = None

    def _stored_name(self, selector):
        identity = selector[len("component:"):]
        if not _identifier(identity):
            raise AssertionError("Malformed saved equipment selector")
        if self.configuration is None:
            self.configuration = self.dut.request("/api/v2/config")
        config = self.configuration
        if (not isinstance(config, dict) or not isinstance(config.get("components"), list)
                or config.get("consortium_id") != self.system["consortium_id"]
                or not isinstance(self.status.get("expected_config_revision"), str)
                or not isinstance(self.status.get("expected_presentation_revision"), str)
                or config.get("config_revision") != self.status["expected_config_revision"]
                or config.get("presentation_revision") != self.status["expected_presentation_revision"]):
            raise AssertionError("Saved equipment configuration does not match the observed system")
        selected = [item for item in config["components"]
                    if isinstance(item, dict) and item.get("component_id") == identity]
        if len(selected) != 1:
            raise AssertionError("Saved equipment identity is missing or ambiguous")
        names = selected[0].get("qualified_names")
        if not isinstance(names, list) or len(names) != 1:
            raise AssertionError("Saved equipment needs exactly one qualified name")
        return _qualified(names[0])

    def resolve(self, row):
        if not isinstance(row, dict) or not isinstance(row.get("name"), str):
            raise AssertionError("Equipment row has no typed selector")
        selector = row["name"]
        selected_name = self._stored_name(selector) if selector.startswith("component:") else None
        if selected_name is None and re.fullmatch(r"native:[0-9a-f]{64}", selector) is None:
            raise AssertionError("Unknown or malformed equipment selector")
        observations = []
        for observation in self.system["observations"]:
            # The editor projects current observations; historical stale
            # entries for the same qualified name must not make that identity
            # ambiguous. More than one current observation still fails.
            if (not isinstance(observation, dict) or observation.get("resolved") is not True
                    or observation.get("stale") is not False):
                continue
            try:
                name = _qualified(observation.get("qualified_name"))
            except AssertionError:
                continue
            if (name == selected_name if selected_name is not None else _native_selector(name) == selector):
                observations.append((observation, name))
        if len(observations) != 1:
            raise AssertionError("Equipment observation is missing or ambiguous")
        observation, name = observations[0]
        if name["consortium_id"] != self.system["consortium_id"]:
            raise AssertionError("Equipment observation belongs to another system")
        runtime = _qualified(observation.get("runtime_address"), runtime=True)
        if any(runtime[key] != name[key] for key in _PORT_KEYS):
            raise AssertionError("Observed runtime address does not match the qualified equipment port")
        if (not _integer(row.get("address"), 0, 14)
                or not _integer(row.get("physical_address"), 0, 0xfffe)
                or not _integer(row.get("type"), 0, 7) or row["type"] == 2
                or not _integer(observation.get("kind"), 0, 7)
                or runtime["logical_address"] != row["address"]
                or observation["kind"] != row["type"] or row.get("bus") != name["side"]):
            raise AssertionError("Equipment row disagrees with its observed runtime address or type")
        return row, name, runtime


def _observed_matches(resolved, component, endpoint):
    row, name, _ = resolved
    return (name["osd_name"] == component.name
            and name["octopus_id"] == endpoint.dut.identity.device_id
            and name["tentacle_id"] == endpoint.tentacle and name["side"] == endpoint.direction
            and row["address"] == component.address.logical
            and row["physical_address"] == component.address.physical
            and row["type"] == component.device_type)

def _matching_devices(state, components):
    """Require the complete identity of each fixture, on its actual DUT side."""
    matches = []
    for component in components:
        expected = {
            "title": component.name, "bus": component.direction,
            "address": component.address.logical,
            "physical_address": component.address.physical,
            "type": component.device_type,
        }
        rows = [row for row in state.get("devices", [])
                if all(row.get(key) == value for key, value in expected.items())]
        if len(rows) != 1:
            return None
        matches.append(rows[0])
    return matches


def _fresh_discovery(records, component, initiator, started_ns):
    """Pair actual DUT identity requests with successful component responses."""
    traffic = [record for record in records
               if record["component"] == component.name
               and record["gpio"] == component.gpio
               and record.get("transport", "wire") == "wire"
               and record["timestamp"] * 1_000_000_000 >= started_ns
               and not record.get("dropped", False)]
    for query, response in ((0x46, 0x47), (0x83, 0x84)):
        requests = [record for record in traffic
                    if record["direction"] == "rx" and record["opcode"] == query
                    and record["source"] == initiator
                    and record["destination"] == component.address.logical
                    and record.get("wire_timestamp_ns", started_ns) >= started_ns]
        if not any(reply["direction"] == "tx" and reply["opcode"] == response
                   and reply["source"] == component.address.logical
                   and reply.get("ok")
                   and reply["timestamp"] >= request["timestamp"]
                   and reply["destination"] == (initiator if response == 0x47 else 15)
                   for request in requests for reply in traffic):
            return False
    return True


def _wire_initiators(records, components, started_ns):
    """Find the DUT on each isolated wire from its fresh directed queries.

    The UI's legacy aggregate front/back address fields can be unregistered
    even while the owner-local engine is registered. No new DUT API is needed:
    these bench wires contain only the named DUT and our claimed components.
    """
    occupants = {}
    for component in components:
        occupants.setdefault(component.gpio, set()).add(component.address.logical)
    sources = {gpio: set() for gpio in occupants}
    for record in records:
        gpio = record["gpio"]
        if (gpio in sources and record["direction"] == "rx"
                and record.get("transport", "wire") == "wire"
                and record["opcode"] in (0x46, 0x83)
                and not record.get("dropped", False)
                and record.get("wire_timestamp_ns", started_ns) >= started_ns
                and record["timestamp"] * 1_000_000_000 >= started_ns
                and record["destination"] in occupants[gpio]
                and 0 <= record["source"] < 15
                and record["source"] not in occupants[gpio]):
            sources[gpio].add(record["source"])
    return {gpio: next(iter(values)) for gpio, values in sources.items() if len(values) == 1}


@dataclass
class ScanResult:
    """UI state and physical evidence from one scan, retained even on failure.

    ``started`` uses the harness process's monotonic clock in nanoseconds.
    ``devices`` contains the UI's unmodified device rows, including an empty or
    partial discovery result. ``assert_contains`` checks only its explicitly
    requested fixture identities and their fresh wire exchanges together.
    The instance is available as ``dut.last_scan`` while a scan is in progress
    and after a timeout or exception, so the runner can retain partial evidence.
    """

    dut: object
    expected: tuple = ()
    devices: list = field(default_factory=list)
    status: dict = field(default_factory=dict)
    records: tuple = ()
    operation: dict = field(default_factory=dict)
    started: int = 0
    initiators: dict = field(default_factory=dict)
    locations: list = field(default_factory=list)
    _session: object = field(default=None, repr=False, compare=False)
    _deadline: float = field(default=0, repr=False, compare=False)
    _boot: str = field(default="", repr=False, compare=False)
    _generation: object = field(default=None, repr=False, compare=False)

    def _retain(self, state=None):
        if state is not None:
            self.status = deepcopy(state)
            self.devices = deepcopy(state.get("devices", []))
        if self._session is not None:
            self.records = tuple(dict(record) for record in self._session.records)
        if self.started:
            self.initiators = _wire_initiators(self.records, self.expected, self.started)

    def _remaining(self):
        remaining = self._deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("The HDMI scan deadline expired")
        return remaining

    def _matching_rows(self, components):
        matches = _matching_devices(self.status, components)
        if matches is not None:
            return matches
        # Persisted equipment labels are UI presentation, not the OSD selector.
        # Only those rows need the existing config API to recover the name.
        rows = self.status.get("devices", [])
        if not any(isinstance(row, dict) and str(row.get("name", "")).startswith("component:") for row in rows):
            return None
        catalog = _ObservedEquipment(self.dut, self.status)
        resolved = [catalog.resolve(row) for row in rows]
        matches = []
        for component in components:
            candidates = [row for row, name, runtime in resolved
                          if _observed_matches((row, name, runtime), component, component.endpoint)]
            if len(candidates) != 1:
                return None
            matches.append(candidates[0])
        return matches

    def _contains(self, components):
        if self._matching_rows(components) is None:
            return False
        return all(component.gpio in self.initiators
                   and _fresh_discovery(self.records, component,
                                        self.initiators[component.gpio], self.started)
                   for component in components)

    def _observe(self, state):
        self._session.assert_healthy()
        self._retain(state)
        if state.get("expected_boot_id") != self._boot:
            raise AssertionError("DUT restarted during the HDMI scan")

    def assert_contains(self, *components):
        """Require requested identities and fresh physical RX/TX, awaiting late metadata.

        Polling uses the original scan's remaining deadline and its live
        session. A completed scan with absent equipment is a normal result;
        only this explicit assertion requires the selected equipment to appear.
        """
        if not self.started:
            raise AssertionError("The HDMI scan has not started")
        for component in components:
            if component not in self.expected or component.dut is not self.dut:
                raise AssertionError(f"{component.name} was not a powered component attached to {self.dut.name} for this scan")
        if (not self.status.get("discovering", False)
                and self.status.get("discovery_generation") != self._generation
                and self.status.get("expected_boot_id") == self._boot
                and self._contains(components)):
            return self
        stopped = getattr(self._session, "_stop", None)
        if self._session is None or stopped is not None and stopped.is_set():
            raise AssertionError("The HDMI scan session ended before the requested identities were confirmed")

        def present(state):
            self._observe(state)
            return (not state.get("discovering", False)
                    and state.get("discovery_generation") != self._generation
                    and self._contains(components))

        try:
            self.dut.wait_for(present, timeout=self._remaining())
            return self
        finally:
            self._retain()

    def assert_contains_only(self, *components):
        """Require exactly the requested devices across the complete UI result."""
        if len({id(component) for component in components}) != len(components):
            raise AssertionError("The exact scan assertion repeats a component")
        if not isinstance(self.status.get("devices"), list) or any(not isinstance(row, dict) for row in self.status["devices"]):
            raise AssertionError("The HDMI scan has no valid equipment list")
        self.assert_contains(*components)
        rows = self.status.get("devices")
        if not isinstance(rows, list) or len(rows) != len(components):
            raise AssertionError("The HDMI scan has extra or missing equipment")
        if not rows:
            return self
        catalog = _ObservedEquipment(self.dut, self.status)
        resolved = [catalog.resolve(row) for row in rows]
        selected = []
        for component in components:
            matches = [index for index, item in enumerate(resolved)
                       if _observed_matches(item, component, component.endpoint)]
            if len(matches) != 1:
                raise AssertionError("The HDMI scan equipment set has missing, duplicate or mismatched identities")
            selected.append(matches[0])
        if len(set(selected)) != len(rows):
            raise AssertionError("The HDMI scan equipment identities are not one-to-one")
        return self

    def assert_on(self, component, endpoint):
        """Assert the observed owner, HDMI adapter and CEC side using typed UI data."""
        if not isinstance(endpoint, TentacleEndpoint):
            raise TypeError("An equipment location assertion needs a DUT tentacle endpoint")
        if not isinstance(self.status.get("devices"), list) or any(not isinstance(row, dict) for row in self.status["devices"]):
            raise AssertionError("The HDMI scan has no valid equipment list")
        self.assert_contains(component)
        rows = self.status.get("devices")
        if not isinstance(rows, list):
            raise AssertionError("The HDMI scan has no equipment list")
        catalog = _ObservedEquipment(self.dut, self.status)
        resolved = [catalog.resolve(row) for row in rows]
        matches = [item for item in resolved if _observed_matches(item, component, endpoint)]
        if len(matches) != 1:
            raise AssertionError("The equipment was not observed on the requested Amity, adapter and CEC side")
        _, name, runtime = matches[0]
        location = {"component": component.name, "qualified_name": name, "runtime_address": runtime}
        if location not in self.locations:
            self.locations.append(deepcopy(location))
        return self


def scan_hdmi(dut, session, *, timeout=90):
    """Return a DUT's actual scan results using only existing UI operations.

    The caller owns the active CEC session and authentication lifetime. This
    function creates no equipment, changes no saved activities, and never
    catches a failure to turn it into a test result. The runner owns reporting.
    """
    if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("Scan timeout must be positive and finite")
    expected = tuple(component for component in session.components
                     if component.dut is dut and component.power.value != "off")
    result = ScanResult(dut, expected=expected, _session=session,
                        _deadline=time.monotonic() + timeout)
    dut.last_scan = result

    try:
        session.assert_healthy()
        system = dut.get_system()
        if system.get("octopus_id") != dut.identity.device_id:
            raise AssertionError(f"DUT HTTP identity does not match {dut.name} in TEST-HARNESS.md")

        def idle(state):
            session.assert_healthy()
            result._retain(state)
            return not state.get("discovering", False)

        before = dut.wait_for(idle, timeout=result._remaining())
        result._generation = before["discovery_generation"]
        result._boot = before["expected_boot_id"]
        result.started = time.monotonic_ns()
        result.operation = dut.start_hdmi_scan()
        saw_discovering = False

        def complete(state):
            nonlocal saw_discovering
            result._observe(state)
            if state.get("discovering", False):
                saw_discovering = True
                return False
            if state.get("discovery_generation") == result._generation:
                return False
            # Submission itself increments a UI generation before the owner
            # begins wire work. Never treat that acknowledgement as completion.
            # If polling missed busy, an actual full identity exchange proves
            # that owner-local discovery ran; absent peers need not answer.
            return saw_discovering or any(component.gpio in result.initiators
                       and _fresh_discovery(result.records, component,
                                            result.initiators[component.gpio], result.started)
                       for component in expected)

        dut.wait_for(complete, timeout=result._remaining())
        session.assert_healthy()
        return result
    finally:
        result._retain()
