"""Generic HDMI discovery through Amity's existing authenticated UI controls.

The framework adds no firmware test API. Scan assertions require the ordinary
UI result and fresh, physically observed identity exchanges on the DUT's wires;
virtual component traffic cannot establish a successful physical scan.
"""

from copy import deepcopy
from dataclasses import dataclass, field
import math
import time

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

    def _contains(self, components):
        if _matching_devices(self.status, components) is None:
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
