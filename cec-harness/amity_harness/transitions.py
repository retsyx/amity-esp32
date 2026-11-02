"""Physical command ordering and shared-device preservation during a handoff.

Linux RX timestamps describe completed frames. This checks their completion
order across an owner's isolated wires; it does not measure the first edge of
a frame or qualify electrical timing between wires.
"""

import threading
import time


class _WireObservation:
    def __init__(self, components):
        self.components = tuple(dict.fromkeys(components))
        if not self.components:
            raise ValueError("An activity wire observation needs equipment")
        self._session = None
        self._condition = threading.Condition()
        self._frames = []
        self._invalid_clock = False
        self._checked = self._closed = False

    def _validate(self, boundary):
        contexts = [component._wire_context(boundary) for component in self.components]
        session = contexts[0][0]
        if any(context[0] is not session for context in contexts):
            raise AssertionError("Activity observation equipment belongs to different CEC sessions")
        if self._session is not None and self._session is not session:
            raise AssertionError("The activity observation CEC session changed")
        session.assert_healthy()
        if session._stop.is_set():
            raise AssertionError("The CEC session ended during the activity observation")
        return session, {component: context[2] for component, context in zip(self.components, contexts)}

    def __enter__(self):
        if self._session is not None or self._closed:
            raise RuntimeError("An activity wire observation cannot be reused")
        marker = time.monotonic_ns()
        self._session, self._initiators = self._validate(marker)
        with self._session._lock:
            self._started = time.monotonic_ns()
            for component in self.components:
                component._wire_observers.add(self._observe)
        return self

    def _observe(self, component, frame, transport):
        if transport != "wire" or frame.source != self._initiators[component]:
            return
        with self._condition:
            if type(frame.timestamp_ns) is not int or frame.timestamp_ns <= 0:
                self._invalid_clock = True
            elif frame.timestamp_ns >= self._started:
                self._frames.append((component, frame))
            self._condition.notify_all()

    @staticmethod
    def _power(component, frame):
        if frame.destination not in (component.address.logical, 15):
            return None
        if frame.opcode == 0x36 and not frame.operands:
            return "standby"
        if frame.destination != component.address.logical:
            return None
        if frame.opcode in (0x04, 0x0d) and not frame.operands:
            return "on"
        if frame.opcode == 0x44 and len(frame.operands) == 1:
            return {0x6d: "on", 0x6c: "off", 0x40: "toggle", 0x6b: "toggle"}.get(frame.operands[0])
        return None

    def _completion_upper(self, component, frame):
        # SSH maps remote kernel time to the lower end of its clock interval.
        # Independent wire workers can have different uncertainty; compare the
        # earlier upper bound with the later lower bound, never midpoint guesses.
        backend = self._session._backends.get(component.gpio)
        uncertainty = getattr(backend, "clock_uncertainty_ns", 0)
        if type(uncertainty) is not int or uncertainty < 0:
            raise AssertionError("Handoff wire clock uncertainty is unavailable")
        return frame.timestamp_ns + 2 * uncertainty

    def _complete(self, since):
        if self._session is None or self._closed or self._checked:
            raise RuntimeError("Assert one active activity wire observation")
        session, initiators = self._validate(since)
        started = getattr(since, "started_ns", None)
        if type(started) is not int or started < self._started or initiators != self._initiators:
            raise AssertionError("The activity action predates its wire observation")
        ended = time.monotonic_ns()
        deadline = time.monotonic() + min(component._power_timeout for component in self.components)
        wires = {component.gpio for component in self.components}
        while True:
            session.assert_healthy()
            if session._stop.is_set() or any(component._session is not session for component in self.components):
                raise AssertionError("The CEC session ended during the activity observation")
            with session._lock:
                drained = all(session._wire_idle_since_ns.get(gpio, 0) >= ended for gpio in wires)
            if drained:
                break
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("Activity wire receive coverage did not drain")
            with self._condition:
                self._condition.wait(min(remaining, 0.02))
        self._validate(since)
        with self._condition:
            if self._invalid_clock:
                raise AssertionError("Activity observation received a physical frame without a wire timestamp")
            frames = [(component, frame) for component, frame in self._frames
                      if started <= frame.timestamp_ns <= ended]
        return session, started, ended, wires, frames

    def __exit__(self, exc_type, exc, traceback):
        if self._session is not None:
            with self._session._lock:
                for component in self.components:
                    component._wire_observers.discard(self._observe)
        self._closed = True
        if exc_type is None and not self._checked:
            raise RuntimeError("The activity wire observation was not asserted")


class ActivityHandoff(_WireObservation):
    """Observe original wire traffic before submitting a local activity change.

    Ordinary power/input assertions still prove that the commands were applied.
    This observation additionally rejects out-of-order commands and transient
    power commands to shared equipment, even when a responder hook hides them.
    """

    def __init__(self, previous, selected, *, shared, receiver, source):
        self.previous, self.selected = previous, selected
        self.shared = tuple(shared)
        self.receiver, self.source = receiver, source
        super().__init__((previous, selected, *self.shared, receiver, source))
        if previous is selected or not self.shared:
            raise ValueError("A handoff needs different displays and shared equipment")
        if any(component.dut is not previous.dut for component in self.components):
            raise ValueError("Handoff ordering applies independently within one Amity")

    def assert_ordered(self, *, since):
        """Require old Standby, selected power-on, then source routing.

        Observe shared equipment throughout the submitted action, and drain
        each receiver through the assertion boundary before accepting absence.
        No fixed quiet period is added to firmware activity execution.
        """
        session, started, ended, wires, frames = self._complete(since)
        with self._condition:
            for component, frame in frames:
                if component in self.shared and self._power(component, frame) is not None:
                    raise AssertionError("Shared equipment received a power command during handoff")
            old = [frame for component, frame in frames if component is self.previous
                   and frame.destination == component.address.logical and self._power(component, frame) == "standby"]
            new = [frame for component, frame in frames if component is self.selected
                   and self._power(component, frame) == "on"]
            route = [frame for component, frame in frames if component is self.receiver
                     and frame.destination == 15 and frame.opcode == 0x86
                     and frame.operands == self.source.address.physical.to_bytes(2, "big")]
            if not old or not new or not route:
                raise AssertionError("Handoff lacks fresh physical Standby, power-on or source routing")
            if (max(self._completion_upper(self.previous, frame) for frame in old) >= min(frame.timestamp_ns for frame in new)
                    or max(self._completion_upper(self.selected, frame) for frame in new) >= min(frame.timestamp_ns for frame in route)):
                raise AssertionError("Handoff physical commands completed out of order")
            proof = {"kind": "wire_handoff", "timestamp_ns": time.monotonic_ns(),
                     "since_monotonic_ns": started, "observation_end_monotonic_ns": ended,
                     "previous": self.previous.name, "selected": self.selected.name,
                     "shared": [component.name for component in self.shared],
                     "standby_wire_timestamp_ns": max(frame.timestamp_ns for frame in old),
                     "power_on_wire_timestamp_ns": min(frame.timestamp_ns for frame in new),
                     "routing_wire_timestamp_ns": min(frame.timestamp_ns for frame in route),
                     "standby_completion_upper_ns": max(self._completion_upper(self.previous, frame) for frame in old),
                     "power_on_completion_upper_ns": max(self._completion_upper(self.selected, frame) for frame in new),
                     "idle_receive_since_ns": {str(gpio): session._wire_idle_since_ns[gpio] for gpio in wires},
                     "observed_wires": sorted(wires), "timestamp_kind": "frame_completion"}
            self.receiver._routing_checks.append(proof)
            self.receiver._trace("component.handoff.assert_passed", **proof)
            self._checked = True
        return self


class _NoStandby(_WireObservation):
    def assert_preserved(self, *, since):
        """Reject physical Standby, power-off or toggle during a repair."""
        session, started, ended, wires, frames = self._complete(since)
        if any(self._power(component, frame) in ("standby", "off", "toggle") for component, frame in frames):
            raise AssertionError("Selected equipment received Standby during activity repair")
        proof = {"kind": "wire_no_standby", "timestamp_ns": time.monotonic_ns(),
                 "since_monotonic_ns": started, "observation_end_monotonic_ns": ended,
                 "idle_receive_since_ns": {str(gpio): session._wire_idle_since_ns[gpio] for gpio in wires},
                 "observed_wires": sorted(wires)}
        for component in self.components:
            with component._power_condition:
                component._power_checks.append(dict(proof))
            component._trace("component.repair_power.assert_passed", **proof)
        self._checked = True
        return self


def observe_no_standby(*components):
    """Observe all selected equipment before submitting same-activity repair."""
    return _NoStandby(components)
