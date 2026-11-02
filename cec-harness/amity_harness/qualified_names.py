"""Physical negative evidence for qualified equipment selection."""

import math
import threading
import time

from .trace import event


_CONTROL_OPCODES = frozenset((0x04, 0x0d, 0x36, 0x44, 0x45,
                              0x80, 0x81, 0x82, 0x86))


class _NoControlObservation:
    """Require inactive fixture wires to receive no DUT power, route or keys.

    Observe original frames before component hooks, independently of the
    bounded transcript ring. ``assert_clear`` includes a CEC-timeout tail and
    waits until receive coverage drains past its original end boundary.
    """

    def __init__(self, scan, *components, duration=None):
        if not components or len({id(component) for component in components}) != len(components):
            raise ValueError("Observe distinct inactive components")
        if any(component not in scan.expected for component in components):
            raise ValueError("Inactive components must belong to the proven scan")
        self.scan, self.components = scan, tuple(components)
        self.duration = scan.dut._runtime.cec_timeout if duration is None else duration
        if (isinstance(self.duration, bool) or not isinstance(self.duration, (int, float))
                or not math.isfinite(self.duration) or self.duration <= 0):
            raise ValueError("Control observation needs a positive finite duration")
        self._session = scan._session
        self._initiators = dict(scan.initiators)
        self._condition = threading.Condition()
        self._started = self._end = 0
        self._forbidden = None
        self._invalid_clock = self._checked = self._closed = False
        self._count = 0
        self._last_frame = {}

    def _validate(self):
        bed = self.scan._testbed
        if (self.scan.dut.last_scan is not self.scan or self.scan.dut._testbed is not bed
                or bed.session is not self._session or not bed._entered or bed._closed
                or self.scan._session is not self._session
                or self.scan.initiators != self._initiators
                or self._session._stop.is_set()):
            raise AssertionError("Control observation belongs to a different active TestBed or scan")
        self._session.assert_healthy()
        self.scan.validate_owners(self.components)
        for component in self.components:
            session, _, source, _ = component._wire_context(self._started or time.monotonic_ns())
            if (session is not self._session or component.dut._testbed is not bed
                    or source != self._initiators.get(component.gpio)):
                raise AssertionError("Control observation changed its physical owner or initiator")

    def __enter__(self):
        if self._started or self._closed:
            raise RuntimeError("A control observation cannot be reused")
        self._validate()
        with self._session._lock:
            self._started = time.monotonic_ns()
            for component in self.components:
                component._wire_observers.add(self._observe)
        event("control.absence.start", wires=sorted({component.gpio for component in self.components}),
              since_monotonic_ns=self._started)
        return self

    def _observe(self, component, frame, transport):
        if transport != "wire":
            return
        with self._condition:
            if frame.timestamp_ns <= 0:
                self._invalid_clock = True
            elif frame.timestamp_ns >= self._started and (not self._end or frame.timestamp_ns <= self._end):
                identity = (frame.timestamp_ns, frame.raw)
                if self._last_frame.get(component.gpio) != identity:
                    self._last_frame[component.gpio] = identity
                    self._count += 1
                if (self._forbidden is None and frame.source == self._initiators[component.gpio]
                        and frame.opcode in _CONTROL_OPCODES):
                    self._forbidden = {"gpio": component.gpio, "component": component.name,
                                       "owner_id": component.dut.identity.device_id,
                                       "frame": frame.raw.hex(":"),
                                       "wire_timestamp_ns": frame.timestamp_ns}
            self._condition.notify_all()

    def _assert_healthy(self):
        if self._session._stop.is_set() or any(component._session is not self._session
                                              for component in self.components):
            raise AssertionError("The CEC session ended during control observation")
        if self._invalid_clock:
            raise AssertionError("Control observation received a physical frame without a wire timestamp")
        if self._forbidden is not None:
            event("control.absence.failed", **self._forbidden)
            raise AssertionError("The DUT sent power, routing or a key to an inactive owner's wire")

    def assert_clear(self):
        if not self._started or self._checked or self._closed:
            raise RuntimeError("Complete one active control observation")
        self._end = time.monotonic_ns() + int(self.duration * 1_000_000_000)
        inspection_end = self._end + int(self.duration * 1_000_000_000)
        wires = {component.gpio for component in self.components}
        while True:
            self._session.assert_healthy()
            with self._condition:
                self._assert_healthy()
                now = time.monotonic_ns()
                if now >= self._end and all(self._session._wire_idle_since_ns.get(gpio, 0) >= self._end
                                             for gpio in wires):
                    break
                if now >= inspection_end:
                    raise TimeoutError("Inactive-wire receive coverage did not drain after control observation")
                self._condition.wait(min((inspection_end - now) / 1_000_000_000, 0.05))
        self._validate()
        with self._condition:
            self._assert_healthy()
            proof = {"kind": "no_owner_control", "owner_ids": sorted({component.dut.identity.device_id
                                                                        for component in self.components}),
                     "wires": sorted(wires), "since_monotonic_ns": self._started,
                     "observation_end_monotonic_ns": self._end,
                     "idle_receive_since_ns": {str(gpio): self._session._wire_idle_since_ns[gpio]
                                               for gpio in wires},
                     "observed_frame_count": self._count}
            bed = self.scan._testbed
            if not hasattr(bed, "control_observations"):
                bed.control_observations = []
            bed.control_observations.append(proof)
            self._checked = True
        event("control.absence.assert_passed", **proof)
        return self

    def __exit__(self, exc_type, exc, traceback):
        with self._session._lock:
            for component in self.components:
                component._wire_observers.discard(self._observe)
        self._closed = True
        if exc_type is None and not self._checked:
            raise RuntimeError("Control observation was not asserted")
        return False


def observe_no_control(scan, *components, duration=None):
    """Observe inactive equipment through operations and a bounded CEC tail."""
    return _NoControlObservation(scan, *components, duration=duration)
