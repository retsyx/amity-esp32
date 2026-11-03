"""Adapter identity evidence from the existing UI scan and physical CEC wire."""

from copy import deepcopy
import json
import threading
import time

from .cec import Frame, TxResult
from .topology import format_physical_address, physical_address
from .trace import event


class AdapterAddressEvidence:
    """Observe one live settings change, then query the selected CEC identity.

    Enter before saving settings. A changed selection must announce its new
    address without this helper prompting it. Both changed and unchanged
    selections must answer a fresh Give Physical Address on the proven owner's
    front wire. Original received frames are observed before responder hooks.
    """

    def __init__(self, dut, front, scan):
        if front.direction != "front" or scan.dut is not dut:
            raise ValueError("Adapter address evidence needs the scan's main and a front component")
        self.dut, self.front, self.scan = dut, front, scan
        self.session = scan._session
        self._witnesses = tuple(component for component in scan.expected
                                if component.dut is front.dut and component.tentacle == front.tentacle)
        self._condition = threading.Condition()
        self._frames, self._queries = [], []
        self._invalid_clock = self._entered = self._closed = self._checked = False

    def _validate(self):
        self.session.assert_healthy()
        if (self.session._stop.is_set() or self.dut.last_scan is not self.scan
                or self.front._session is not self.session
                or self.scan._session is not self.session or self.front not in self.scan.expected):
            raise AssertionError("Adapter address evidence lost its active owner scan")
        self.scan.validate_owners((self.front,))
        context = self.front._wire_context(time.monotonic_ns())
        if context[0] is not self.session or context[2] != self.scan.initiators.get(self.front.gpio):
            raise AssertionError("Adapter address evidence has no matching physical initiator")
        if self._entered and (context[2] != self._initiator
                              or self.scan.owner_boots != self._owner_boots):
            raise AssertionError("Adapter address evidence changed owner boot or initiator")
        return context[2]

    def _state(self, *, check=None):
        state = self.dut.get_state()
        discovering = state.get("discovering")
        if check is not None:
            check["aggregate_discovering_after"] = discovering if type(discovering) is bool else None
        if (state.get("expected_boot_id") != self.scan._boot or type(discovering) is not bool
                or not isinstance(state.get("devices"), list)
                or type(state.get("discovery_generation")) is not int):
            raise AssertionError("Adapter settings returned an invalid main boot or discovery state")
        # This flag aggregates every owner's adapters. A previous explicit UI
        # scan may still be finishing elsewhere; it says nothing about whether
        # this settings change enumerated the target adapter's physical wires.
        # The editor's rows are the current local and remote equipment cache.
        # Their order is presentation; the actual cached identities must stay.
        devices = [deepcopy(row) for row in state["devices"]]
        devices.sort(key=lambda row: json.dumps(row, sort_keys=True))
        return state["discovery_generation"], devices, discovering

    def __enter__(self):
        if self._entered or self._closed:
            raise RuntimeError("Adapter address evidence cannot be reused")
        self._initiator = self._validate()
        self._owner_boots = dict(self.scan.owner_boots)
        self._generation, self._cache, self._aggregate_discovering = self._state()
        with self.session._lock:
            self._started = time.monotonic_ns()
            for component in self._witnesses:
                component._wire_observers.add(self._observe)
        self._entered = True
        self._report = {"kind": "adapter_physical_address", "component": self.front.name,
                        "owner": self.front.dut.identity.device_id, "gpio": self.front.gpio,
                        "owner_boots": dict(self._owner_boots),
                        "started_monotonic_ns": self._started, "checks": []}
        bed = self.scan._testbed
        if not hasattr(bed, "control_observations"):
            bed.control_observations = []
        bed.control_observations.append(self._report)
        return self

    def _observe(self, component, frame, transport):
        if (component not in self._witnesses or transport != "wire"
                or frame.source != self.scan.initiators.get(component.gpio)):
            return
        identity = component is self.front and frame.opcode == 0x84
        enumeration = frame.destination == component.address.logical and frame.opcode in (0x83, 0x46)
        if not identity and not enumeration:
            return
        with self._condition:
            if type(frame.timestamp_ns) is not int or frame.timestamp_ns <= 0:
                self._invalid_clock = True
            elif frame.timestamp_ns >= self._started:
                if identity:
                    self._frames.append(frame)
                else:
                    self._queries.append({"component": component.name, "gpio": component.gpio,
                                          "frame": frame.raw.hex(":"), "wire_timestamp_ns": frame.timestamp_ns})
            self._condition.notify_all()

    def _reports(self, expected):
        with self._condition:
            if self._invalid_clock:
                raise AssertionError("Adapter identity report has no physical wire timestamp")
            frames = tuple(self._frames)
            queries = deepcopy(self._queries)
        if queries:
            self._report["checks"][-1]["unexpected_queries"] = queries
            raise AssertionError("Adapter settings caused unsolicited equipment discovery queries")
        if any(frame.raw != expected.raw for frame in frames):
            raise AssertionError("Adapter announced an unexpected physical address or identity")
        return frames

    def _wait_report(self, expected, *, after, deadline_ns):
        deadline = time.monotonic() + self.front._power_timeout
        while True:
            self.session.assert_healthy()
            if self.session._stop.is_set() or self.front._session is not self.session:
                raise AssertionError("The CEC session ended during adapter identity evidence")
            reports = self._reports(expected)
            matches = [frame for frame in reports if after < frame.timestamp_ns <= deadline_ns]
            if matches:
                return matches[0]
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("Adapter lacks a fresh physical address report")
            with self._condition:
                self._condition.wait(min(remaining, .02))

    def _drain(self, through):
        deadline = time.monotonic() + self.front._power_timeout
        while True:
            self.session.assert_healthy()
            if self.session._stop.is_set() or self.front._session is not self.session:
                raise AssertionError("The CEC session ended during adapter identity evidence")
            with self.session._lock:
                if all(self.session._wire_idle_since_ns.get(component.gpio, 0) >= through
                       for component in self._witnesses):
                    return
            if time.monotonic() >= deadline:
                raise TimeoutError("Adapter identity wire receive coverage did not drain")
            with self._condition:
                self._condition.wait(.02)

    def assert_selected(self, address, *, changed):
        """Require the selected PA on the actual front wire without rebooting."""
        if not self._entered or self._closed or self._checked:
            raise RuntimeError("Assert one active adapter address observation")
        if type(changed) is not bool:
            raise ValueError("Changed must state whether the selected address changed")
        selected = physical_address(address)
        expected = Frame(self._initiator, 15, 0x84, selected.to_bytes(2, "big") + b"\x04")
        check = {"selected_address": format_physical_address(selected).upper(),
                 "changed": changed, "passed": False,
                 "aggregate_discovering_before": self._aggregate_discovering}
        self._report["checks"].append(check)
        self._state(check=check)
        self._validate()
        if changed:
            deadline_ns = time.monotonic_ns() + int(self.front._power_timeout * 1e9)
            announcement = self._wait_report(expected, after=self._started, deadline_ns=deadline_ns)
            check.update(announcement_frame=announcement.raw.hex(":"),
                         announcement_wire_timestamp_ns=announcement.timestamp_ns)

        query = self.front.send(self._initiator, 0x83)
        requested = Frame(self.front.address.logical, self._initiator, 0x83)
        if (not isinstance(query, TxResult) or not query.ok or query.transport != "wire"
                or query.frame.raw != requested.raw or type(query.timestamp_ns) is not int
                or query.timestamp_ns < self._started):
            raise AssertionError("Adapter identity requires a successful fresh physical query")
        check.update(query_frame=query.frame.raw.hex(":"), query_wire_timestamp_ns=query.timestamp_ns)
        response_deadline_ns = query.timestamp_ns + int(self.front._power_timeout * 1e9)
        reply = self._wait_report(expected, after=query.timestamp_ns, deadline_ns=response_deadline_ns)
        check.update(reply_frame=reply.raw.hex(":"), reply_wire_timestamp_ns=reply.timestamp_ns)
        through = time.monotonic_ns()
        self._drain(through)
        self._validate()
        frames = self._reports(expected)
        generation, devices, _ = self._state(check=check)
        check.update(expected_discovery_generation=self._generation,
                     observed_discovery_generation=generation, cache_unchanged=devices == self._cache,
                     cache_entry_count=len(self._cache))
        if generation != self._generation or devices != self._cache:
            raise AssertionError("Adapter settings rescanned or changed the equipment cache")
        check.update(observation_end_monotonic_ns=through,
                     idle_receive_since_ns={str(component.gpio): self.session._wire_idle_since_ns[component.gpio]
                                            for component in self._witnesses},
                     reports=[{"frame": frame.raw.hex(":"), "wire_timestamp_ns": frame.timestamp_ns}
                              for frame in frames if frame.timestamp_ns <= through], passed=True)
        self._checked = True
        event("adapter_address.assert_passed", dut=self.dut.name, component=self.front.name,
              owner=self.front.dut.identity.device_id, gpio=self.front.gpio, **check)
        return self

    def __exit__(self, exc_type, exc, traceback):
        if self._entered:
            with self.session._lock:
                for component in self._witnesses:
                    component._wire_observers.discard(self._observe)
        self._closed = True
        if exc_type is None and not self._checked:
            raise RuntimeError("The adapter address observation was not asserted")
