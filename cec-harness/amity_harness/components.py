"""Stateful HDMI-CEC components, configured by each functional test."""

from dataclasses import dataclass
from contextlib import nullcontext
from collections import deque
from enum import Enum
import logging
import math
import threading
import time

from .cec import CecError, Frame, TxResult
from .config import current_config
from .connections import TentacleEndpoint, hdmi_port
from .topology import CecAddress
from .trace import event as trace_event


class PowerState(str, Enum):
    ON = "on"
    STANDBY = "standby"
    OFF = "off"


_POWER_COMMANDS = frozenset(("standby", "image_view_on", "text_view_on",
                             "power_on_function", "power_off_function", "power_toggle"))


@dataclass
class Hooks:
    """Optional component-level monitoring and fault injection.

    before_tx(component, frame) and before_rx(component, frame) return a Frame
    (possibly modified), or None to drop it. after_tx(component, result) observes
    the TX result, whose transport identifies wire or virtual delivery;
    after_rx(component, frame) observes completed handling. Hooks run on both
    transports and may raise to fail the test. Dropping RX suppresses handling,
    while an already powered recipient still ACKs. Physical wire timing/ACK
    faults need kernel pin error injection. Broadcast after_tx runs once with
    the wire result; its independent virtual delivery is recorded separately.
    """

    before_tx: object = None
    after_tx: object = None
    before_rx: object = None
    after_rx: object = None


class _SourceIsolationObservation:
    """Retain the first forbidden original wire frame across two AVR ports."""

    def __init__(self, receiver, source, *, upstream, since, duration=None):
        self.receiver, self.source, self.upstream = receiver, source, upstream
        self.since = since
        self.duration = receiver._power_timeout if duration is None else duration
        if (isinstance(self.duration, bool) or not isinstance(self.duration, (int, float))
                or not math.isfinite(self.duration) or self.duration <= 0):
            raise ValueError("Source isolation needs a positive finite observation duration")
        self._condition = threading.Condition()
        self._session = None
        self._started = self._count = 0
        self._leak = None
        self._invalid_clock = self._checked = self._closed = False

    def _validate(self):
        contexts = [component._wire_context(self.since)
                    for component in (self.receiver, self.upstream, self.source)]
        session = contexts[0][0]
        if any(context[0] is not session for context in contexts):
            raise AssertionError("Source isolation equipment belongs to different CEC sessions")
        if self._session is not None and self._session is not session:
            raise AssertionError("The source isolation CEC session changed")
        session.assert_healthy()
        if getattr(session, "_stop", None) is not None and session._stop.is_set():
            raise AssertionError("The CEC session ended during source isolation observation")
        return session

    def __enter__(self):
        if self._session is not None or self._closed:
            raise RuntimeError("A source isolation observation cannot be reused")
        if (not self.receiver.can_route or not getattr(self.upstream, "can_route", False)
                or len({self.receiver.gpio, self.upstream.gpio, self.source.gpio}) != 3):
            raise ValueError("Source isolation needs two distinct AVR-side wires and an isolated source wire")
        self._session = self._validate()
        # Owner proof performs HTTP outside the responder lock. Register both
        # original-frame observers atomically before the source transmits.
        with self._session._lock:
            self._started = time.monotonic_ns()
            for receiver in (self.receiver, self.upstream):
                receiver._wire_observers.add(self._observe)
        self.receiver._trace("component.source_isolation.start", source_component=self.source.name,
                             physical_address=self.source.address.physical,
                             observed_wires=[self.receiver.gpio, self.upstream.gpio],
                             since_monotonic_ns=self._started, duration_seconds=self.duration)
        return self

    def _observe(self, receiver, frame, transport):
        if transport != "wire":
            return
        with self._condition:
            if frame.timestamp_ns <= 0:
                self._invalid_clock = True
            elif frame.timestamp_ns >= self._started:
                self._count += 1
                target = self.source.address.physical.to_bytes(2, "big")
                routing = (frame.destination == 15 and
                           ((frame.opcode in (0x82, 0x86, 0x81) and frame.operands == target)
                            or (frame.opcode == 0x80 and len(frame.operands) == 4
                                and frame.operands[2:] == target)))
                wake = frame.opcode in (0x04, 0x0d) and not frame.operands
                if self._leak is None and (routing or wake):
                    self._leak = {"frame": frame.raw.hex(":"), "source": frame.source,
                                  "destination": frame.destination, "observed_gpio": receiver.gpio,
                                  "wire_timestamp_ns": frame.timestamp_ns}
            self._condition.notify_all()

    def assert_suppressed(self, announcement):
        """Observe through TX + CEC timeout, independently of the 250 ms reply."""
        if self._session is None or self._closed or self._checked:
            raise RuntimeError("Complete one active source isolation observation")
        self._validate()
        trigger = self.source._activation_timestamp(announcement, exact_component=True)
        if trigger < self._started:
            raise AssertionError("The physical source attempt predates its isolation observation")
        deadline_ns = trigger + int(self.duration * 1_000_000_000)
        inspection_deadline_ns = deadline_ns + int(self.receiver._power_timeout * 1_000_000_000)
        while True:
            self._session.assert_healthy()
            if (any(component._session is not self._session for component in
                    (self.receiver, self.upstream, self.source)) or self._session._stop.is_set()):
                raise AssertionError("The CEC session ended during source isolation observation")
            with self._condition:
                if self._invalid_clock:
                    raise AssertionError("Source isolation received a physical frame without a wire timestamp")
                if self._leak is not None:
                    self.receiver._trace("component.source_isolation.failed", level=logging.ERROR,
                                         trigger_wire_timestamp_ns=trigger, **self._leak)
                    raise AssertionError("The isolated source reached an AVR-side CEC wire")
                remaining = (deadline_ns - time.monotonic_ns()) / 1_000_000_000
                if remaining <= 0:
                    idle = getattr(self._session, "_wire_idle_since_ns", {})
                    if all(idle.get(receiver.gpio, 0) >= deadline_ns
                           for receiver in (self.receiver, self.upstream)):
                        break
                    if time.monotonic_ns() >= inspection_deadline_ns:
                        raise TimeoutError("AVR-side CEC receive coverage did not drain after the observation window")
                    remaining = (inspection_deadline_ns - time.monotonic_ns()) / 1_000_000_000
                self._condition.wait(min(remaining, 0.05))
        # Fresh membership/boot proof cannot restart or shorten the observed
        # interval. Inspect the monitor again after slow owner reads complete.
        self._validate()
        with self._condition:
            if self._leak is not None or self._invalid_clock:
                raise AssertionError("The isolated source reached an AVR-side CEC wire")
            proof = {"kind": "wire_suppression", "command": "source_isolation",
                     "physical_address": self.source.address.physical,
                     "timestamp_ns": time.monotonic_ns(), "since_monotonic_ns": self._started,
                     "trigger_wire_timestamp_ns": trigger, "trigger_frame": announcement.frame.raw.hex(":"),
                     "observation_end_monotonic_ns": deadline_ns,
                     "observed_wires": [self.receiver.gpio, self.upstream.gpio],
                     "idle_receive_since_ns": {str(receiver.gpio): self._session._wire_idle_since_ns[receiver.gpio]
                                               for receiver in (self.receiver, self.upstream)},
                     "observed_frame_count": self._count}
            self.receiver._routing_checks.append(proof)
            self._checked = True
        self.receiver._trace("component.source_isolation.assert_passed", **proof)
        return self.receiver

    def __exit__(self, exc_type, exc, traceback):
        if self._session is not None:
            with self._session._lock:
                for receiver in (self.receiver, self.upstream):
                    receiver._wire_observers.discard(self._observe)
        self._closed = True
        if exc_type is None and not self._checked:
            raise RuntimeError("Source isolation observation was not asserted")


class Component:
    """Generic registered CEC device with identity, power and message hooks.

    The first argument is a DUT tentacle endpoint or another component. Attach
    a child to a component with ``port=`` naming that parent's HDMI input; its
    DUT, tentacle, CEC side and GPIO are inherited. A DUT first argument remains
    a convenience for a direct endpoint, with optional direction/tentacle.
    ``address`` is a CecAddress(logical, physical), or None for assignment.
    Only powered On/Standby devices claim a logical address or receive frames.
    """

    device_type = 4
    logical_address_type = 3
    logical_addresses = (4, 8, 11)
    all_device_types = 0x10
    can_route = False

    def __init__(self, attachment, name, *, tentacle=None, direction=None, address=None,
                 port=None, power=PowerState.STANDBY, vendor_id=1,
                 hooks=None):
        if not isinstance(name, str) or not name:
            raise ValueError("A component needs a name")
        try:
            encoded = name.encode("ascii")
        except UnicodeEncodeError as exc:
            raise ValueError("CEC OSD names must be ASCII") from exc
        if len(encoded) > 14:
            raise ValueError("CEC OSD names cannot exceed 14 bytes")
        if address is not None and not isinstance(address, CecAddress):
            raise TypeError("address must be CecAddress(logical, physical)")
        if isinstance(vendor_id, bool) or not isinstance(vendor_id, int) or not 0 <= vendor_id <= 0xffffff:
            raise ValueError("CEC vendor IDs must fit 24 bits")
        if isinstance(attachment, Component):
            endpoint = attachment.endpoint
            hdmi_port(port)
        elif isinstance(attachment, TentacleEndpoint):
            endpoint = attachment
            if port is not None:
                raise ValueError("An HDMI input port belongs to a parent component, not a DUT endpoint")
        else:
            # Duck typing retains lightweight unit DUTs while the endpoint
            # constructor validates access to the parsed wiring lookup.
            endpoint = TentacleEndpoint(attachment, 0 if tentacle is None else tentacle,
                                        "back" if direction is None else direction)
            attachment = endpoint
            if port is not None:
                raise ValueError("An HDMI input port requires attachment to a parent component")
        if tentacle is not None and (isinstance(tentacle, bool) or not isinstance(tentacle, int)
                                      or tentacle != endpoint.tentacle):
            raise ValueError("Tentacle override conflicts with the component's attachment")
        if direction is not None and direction != endpoint.direction:
            raise ValueError("Direction override conflicts with the component's attachment")
        self._attachment, self._endpoint = attachment, endpoint
        self.name = name
        self.address, self.port = address, port
        self.vendor_id, self.hooks = vendor_id, hooks or Hooks()
        self._power = PowerState(power)
        self._session = None
        self.cec_version = 6
        self._active_since_ns = 0
        self.pressed_key = None
        self._power_revision = 0
        self._power_calls = 0
        self._power_condition = threading.Condition()
        self._power_history = deque(maxlen=256)
        self._power_checks = deque(maxlen=256)
        self._active_input = None
        self._input_revision = self._input_calls = 0
        self._selected_input = None
        self._switch_input_revision = self._switch_input_calls = 0
        self._routing_history = deque(maxlen=256)
        self._routing_checks = deque(maxlen=256)
        self._wire_observers = set()
        runtime = getattr(self.dut, "_runtime", None) or current_config()
        self._power_timeout = runtime.cec_timeout

    @property
    def attachment(self):
        return self._attachment

    @property
    def parent(self):
        return self._attachment if isinstance(self._attachment, Component) else None

    @property
    def endpoint(self):
        return self._endpoint

    @property
    def dut(self):
        return self._endpoint.dut

    @property
    def tentacle(self):
        return self._endpoint.tentacle

    @property
    def direction(self):
        return self._endpoint.direction

    @property
    def gpio(self):
        return self._endpoint.gpio

    @property
    def tv_input(self):
        return self._endpoint.tv_input

    @property
    def power(self):
        return self._power

    @property
    def power_events(self):
        """Bounded snapshots of successfully handled, recognized power frames.

        Entries retain the original received payload/clock and distinguish wire
        from virtual transport. ``applied`` is false if a hook changed the
        payload, the handler did not invoke power control, or later hook work
        overrode the handler's resulting state. Local setters are not commands.
        """
        with self._power_condition:
            return tuple(dict(event) for _, event in self._power_history)

    @property
    def power_assertions(self):
        with self._power_condition:
            return tuple(dict(assertion) for assertion in self._power_checks)

    def checkpoint(self):
        """Mark a local monotonic boundary before an action is submitted."""
        return time.monotonic_ns()

    def _trace(self, name, *, level=logging.INFO, **fields):
        trace_event(name, level=level, dut=getattr(self.dut, "name", None),
                    component=self.name, tentacle=self.tentacle, side=self.direction,
                    gpio=self.gpio, **fields)

    def _activation_timestamp(self, announcement, *, exact_component=False):
        """Correlate a complete physical Active Source TX on this session/wire."""
        session = self._session
        if (session is None or not isinstance(announcement, TxResult) or not isinstance(announcement.frame, Frame) or not announcement.ok
                or announcement.transport != "wire" or not isinstance(announcement.timestamp_ns, int)
                or isinstance(announcement.timestamp_ns, bool) or announcement.timestamp_ns <= 0
                or announcement.frame.opcode != 0x82 or announcement.frame.destination != 15):
            raise AssertionError("A remediation deadline needs a successful physical Active Source transmission")
        senders = [component for component in session.components
                   if component.gpio == self.gpio and component._session is session
                   and component.address.logical == announcement.frame.source
                   and component.address.physical.to_bytes(2, "big") == announcement.frame.operands
                   and (not exact_component or component is self)]
        if len(senders) != 1 or not any(
                record.get("component") == senders[0].name and record.get("gpio") == self.gpio
                and record.get("direction") == "tx" and record.get("transport") == "wire"
                and record.get("ok") is True and record.get("frame") == announcement.frame.raw.hex(":")
                and record.get("wire_timestamp_ns") == announcement.timestamp_ns for record in session.records):
            raise AssertionError("The Active Source transmission belongs to a different component, session or wire")
        return announcement.timestamp_ns

    def _response_timing(self, after, within):
        if after is None and within is None:
            return {}
        if (after is None or isinstance(within, bool) or not isinstance(within, (int, float))
                or not math.isfinite(within) or not 0 < within <= 0.250):
            raise ValueError("Source-theft remediation needs a positive deadline of at most 250 ms")
        return {"trigger_wire_timestamp_ns": self._activation_timestamp(after),
                "trigger_frame": after.frame.raw.hex(":"), "max_elapsed_ns": int(within * 1_000_000_000)}

    @staticmethod
    def _response_in_time(event, timing):
        return not timing or 0 <= event["wire_timestamp_ns"] - timing["trigger_wire_timestamp_ns"] <= timing["max_elapsed_ns"]

    @staticmethod
    def _response_latency(event, timing):
        return {**timing, "response_latency_ns": event["wire_timestamp_ns"] - timing["trigger_wire_timestamp_ns"]} if timing else {}

    def wait_for_standby_after(self, announcement, *, within=0.250):
        """Require directed Standby/state within 250 ms of this player's TX.

        Linux timestamps mark the last transmitted/received byte. Both events
        use the Pi's monotonic wire clock. Identity
        proof may finish later, but cannot make a late CEC response acceptable.
        """
        started = self._activation_timestamp(announcement, exact_component=True)
        return self.wait_for_power(PowerState.STANDBY, since=started, command="standby", directed=True,
                                   after=announcement, within=within)

    def assert_power(self, power):
        """Assert current model state only, for example initial Standby."""
        power = PowerState(power)
        with self._power_condition:
            if self.power != power:
                self._trace("component.power.assert_failed", level=logging.ERROR,
                            expected=power.value, actual=self.power.value, kind="state")
                raise AssertionError(f"{self.name} is {self.power.value}, expected {power.value}")
            self._power_checks.append({"kind": "state", "power": power.value,
                                       "timestamp_ns": time.monotonic_ns()})
            self._trace("component.power.assert_passed", power=power.value, kind="state")
        return self

    def wait_for_power(self, power, *, since, timeout=None, command=None, directed=False, broadcast=False,
                       after=None, within=None):
        """Require a fresh physical DUT power command and its resulting state.

        ``since`` is an activity action boundary or a checkpoint() integer.
        Actual kernel wire time establishes freshness, never a hook's rewritten
        timestamp. A valid command in the same power state still counts. Local
        setters, virtual peer traffic and unrelated wake-up side effects do not.
        ``command='standby'`` specifically requires Standby opcode 0x36 rather
        than another command that also leaves the component in Standby.
        ``directed=True`` excludes broadcasts to all components on the wire.
        ``broadcast=True`` requires the original command to address all devices.
        """
        power = PowerState(power)
        if not isinstance(directed, bool):
            raise ValueError("Directed power assertion must be a boolean")
        if not isinstance(broadcast, bool):
            raise ValueError("Broadcast power assertion must be a boolean")
        if directed and broadcast:
            raise ValueError("Power assertions cannot require both directed and broadcast commands")
        if command is not None and (not isinstance(command, str) or command not in _POWER_COMMANDS):
            raise ValueError("Unknown component power command")
        if power == PowerState.OFF:
            raise ValueError("Off has no CEC power command; use assert_power for an unpowered component")
        timeout = self._power_timeout if timeout is None else timeout
        if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or not math.isfinite(timeout) or timeout <= 0:
            raise ValueError("Power assertion timeout must be a positive finite number")
        timing = self._response_timing(after, within)
        session, started, source, owner_scan = self._wire_context(since)
        deadline = time.monotonic() + timeout
        if timing:
            deadline = min(deadline, (timing["trigger_wire_timestamp_ns"] + timing["max_elapsed_ns"]) / 1_000_000_000)
        self._trace("component.power.wait", power=power.value, command=command,
                    directed=directed, broadcast=broadcast, since_monotonic_ns=started, initiator=source,
                    timeout=timeout, **timing)
        owner_checked = owner_scan is None
        owner_event = None
        while True:
            session.assert_healthy()
            stopped = getattr(session, "_stop", None)
            if self._session is not session or stopped is not None and stopped.is_set():
                raise AssertionError("The CEC session ended before the requested power command was confirmed")
            with self._power_condition:
                matching = next((event for owner, event in reversed(self._power_history)
                                 if owner is session and event["transport"] == "wire"
                                 and event["source"] == source and event["applied"]
                                 and (not directed or event["destination"] == self.address.logical)
                                 and (not broadcast or event["destination"] == 15)
                                 and (command is None or event["command"] == command)
                                 and event["wire_timestamp_ns"] >= started
                                 and event["processed_monotonic_ns"] >= started
                                 and event["requested_power"] == power.value
                                 and event["after"] == power.value
                                 and event["state_revision"] == self._power_revision
                                 and self._response_in_time(event, timing)), None)
                ready = self.power == power and matching is not None
                if ready and owner_checked and (owner_scan is None or matching["state_revision"] == owner_event["state_revision"]):
                    if owner_event is not None:
                        matching = owner_event
                    self._power_checks.append({"kind": "wire_command", "power": power.value,
                                               "timestamp_ns": time.monotonic_ns(),
                                               "since_monotonic_ns": started,
                                               "frame": matching["frame"], "source": source,
                                               "destination": matching["destination"],
                                               "wire_timestamp_ns": matching["wire_timestamp_ns"],
                                               "command": matching["command"],
                                               **self._response_latency(matching, timing)})
                    self._trace("component.power.assert_passed", kind="wire_command",
                                power=power.value, command=matching["command"],
                                frame=matching["frame"], source=source,
                                destination=matching["destination"],
                                since_monotonic_ns=started,
                                wire_timestamp_ns=matching["wire_timestamp_ns"],
                                **self._response_latency(matching, timing))
                    return self
                if ready:
                    # Verify the owner outside component/session locks, then
                    # inspect the unchanged physical effect again before return.
                    recheck_owner = True
                else:
                    recheck_owner = False
                    owner_checked = owner_scan is None
                remaining = deadline - time.monotonic()
                if remaining <= 0 and not (timing and recheck_owner):
                    self._trace("component.power.timeout", level=logging.ERROR,
                                expected=power.value, actual=self.power.value,
                                command=command, since_monotonic_ns=started, **timing)
                    raise TimeoutError(f"{self.name} did not handle a fresh DUT {power.value} command and remain {power.value}")
                # Wake on evidence; periodically check responder health even if
                # a failing worker produces no further component callbacks.
                if not recheck_owner:
                    self._power_condition.wait(min(remaining, 0.05))
            if recheck_owner:
                self._wire_context(since)
                # The physical effect was observed within its CEC wait. A
                # slower HTTP owner proof may confirm that same state later,
                # but cannot admit a changed/restored state after the deadline.
                owner_event = matching
                owner_checked = True

    def _checkpoint_scan(self, session):
        """Locate an explicitly proven coordinator scan for a headless owner."""
        bed = getattr(self.dut, "_testbed", None)
        if bed is not None and getattr(bed, "session", None) is session:
            candidates = []
            for dut in getattr(bed, "duts", ()):
                scan = getattr(dut, "last_scan", None)
                if (scan is not None and getattr(scan, "_session", None) is session
                        and getattr(scan, "_testbed", None) is bed and self in getattr(scan, "expected", ())
                        and getattr(scan, "owner_duts", {}).get(self.dut.identity.device_id) is self.dut
                        and not any(scan is other for other in candidates)):
                    candidates.append(scan)
            if len(candidates) > 1:
                raise AssertionError("The component has ambiguous coordinator scan evidence")
            if candidates:
                return candidates[0]
        return getattr(self.dut, "last_scan", None)

    def _validate_action_owners(self, since, scan, session, *, _system=None):
        bed = getattr(scan, "_testbed", None)
        coordinator = getattr(scan, "dut", None)
        owner = self.dut.identity.device_id
        main = getattr(getattr(coordinator, "identity", None), "device_id", None)
        boots = getattr(scan, "owner_boots", {})
        owners = getattr(scan, "owner_duts", {})
        if (bed is None or getattr(bed, "session", None) is not session
                or not getattr(bed, "_entered", False) or getattr(bed, "_closed", True)
                or getattr(self.dut, "_testbed", None) is not bed
                or getattr(coordinator, "_testbed", None) is not bed
                or getattr(coordinator, "last_scan", None) is not scan
                or getattr(scan, "_session", None) is not session
                or self not in getattr(scan, "expected", ())
                or owners.get(owner) is not self.dut or owners.get(main) is not coordinator
                or boots.get(main) != getattr(scan, "_boot", None)
                or not boots.get(owner)):
            raise AssertionError("Wire owner evidence belongs to a different active TestBed or scan")
        if not isinstance(since, int):
            if (getattr(since, "scan", None) is not scan or getattr(since, "testbed", None) is not bed
                    or getattr(since, "dut", None) is not coordinator
                    or getattr(since, "boot", None) != boots.get(main)
                    or getattr(since, "owner_boots", None) != boots
                    or getattr(since, "owner_duts", None) != owners
                    or getattr(since, "initiators", None) != scan.initiators):
                raise AssertionError("The action has no matching coordinator and member boot evidence")
        validator = getattr(scan, "validate_owners", None)
        if not callable(validator):
            raise AssertionError("The scan cannot verify the current physical owner")
        if _system is None:
            validator((self,))
        else:
            validator((self,), _system=_system)

    def _wire_context(self, since):
        """Validate a boundary and the scan-proven initiator on this wire."""
        session = self._session
        if session is None:
            raise AssertionError("Wire command assertions need an active CEC session")
        if isinstance(since, int) and not isinstance(since, bool):
            started = since
            scan = self._checkpoint_scan(session)
            initiators = getattr(scan, "initiators", {})
            if getattr(scan, "_session", session) is not session:
                raise AssertionError("The DUT identity scan belongs to a different CEC session")
        else:
            started = getattr(since, "started_ns", getattr(since, "started", None))
            scan = getattr(since, "scan", None)
            if getattr(since, "session", None) is not session:
                raise AssertionError("The action boundary belongs to a different DUT or CEC session")
            initiators = getattr(since, "initiators", {})
        if isinstance(started, bool) or not isinstance(started, int) or started <= 0:
            raise ValueError("A wire assertion needs a valid monotonic action boundary")
        source = initiators.get(self.gpio) if hasattr(initiators, "get") else None
        occupied = {component.address.logical for component in session.components if component.gpio == self.gpio}
        if isinstance(source, bool) or not isinstance(source, int) or not 0 <= source < 15 or source in occupied:
            raise AssertionError("A wire assertion needs the DUT initiator verified by its HDMI scan")
        remote = scan is not None and getattr(scan, "dut", self.dut) is not self.dut
        if remote:
            self._validate_action_owners(since, scan, session)
        elif not isinstance(since, int) and getattr(since, "dut", None) is not self.dut:
            raise AssertionError("The action boundary belongs to a different DUT or CEC session")
        return session, started, source, scan if remote else None

    @property
    def active_input(self):
        return self._active_input

    @active_input.setter
    def active_input(self, physical):
        with self._power_condition:
            if physical != self._active_input:
                self._input_revision += 1
            self._active_input = physical
            self._input_calls += 1
            self._power_condition.notify_all()

    @property
    def selected_input(self):
        """Numeric input selected through Select AV Input Function (0x69)."""
        return self._selected_input

    @selected_input.setter
    def selected_input(self, number):
        if isinstance(number, bool) or not isinstance(number, int) or not 0 <= number <= 255:
            raise ValueError("Switch input must be an integer from 0 to 255")
        with self._power_condition:
            if number != self._selected_input:
                self._switch_input_revision += 1
            self._selected_input = number
            self._switch_input_calls += 1
            self._power_condition.notify_all()

    @property
    def routing_events(self):
        with self._power_condition:
            return tuple(dict(event) for _, event in self._routing_history)

    @property
    def routing_assertions(self):
        with self._power_condition:
            return tuple(dict(event) for event in self._routing_checks)

    def wait_for_input(self, source_component, *, since, timeout=None, after=None, within=None):
        """Require a fresh applied DUT Set Stream Path and its current input.

        Peer Active Source broadcasts can change model state; they cannot prove
        Amity restored a route. Hooks cannot rewrite or manufacture wire proof.
        """
        if not self.can_route:
            raise ValueError("This component does not model HDMI input routing")
        if isinstance(source_component, Component):
            if source_component.dut is not self.dut or source_component._session is not self._session:
                raise ValueError("Input equipment belongs to a different DUT or CEC session")
            physical = source_component.address.physical
        else:
            physical = CecAddress(0, source_component).physical
        return self._wait_for_routing(physical, since=since, timeout=timeout, command="set_stream_path", after=after, within=within)

    def wait_for_active_source(self, source_component, *, since, timeout=None):
        """Require a fresh applied DUT Active Source on this receiving wire."""
        if not self.can_route:
            raise ValueError("This component does not model HDMI input routing")
        if not isinstance(source_component, Component):
            raise TypeError("An Active Source assertion needs its source component")
        if source_component.dut is not self.dut or source_component._session is not self._session:
            raise ValueError("Input equipment belongs to a different DUT or CEC session")
        return self._wait_for_routing(source_component.address.physical, since=since,
                                     timeout=timeout, command="active_source")

    def observe_source_isolation(self, source_component, *, upstream, since, duration=None):
        """Watch both AVR-side wires before an isolated source attempts theft.

        The upstream attachment is mandatory: observing the AVR's output wire
        alone cannot prove suppression before a claim reaches its input wire.
        ``assert_suppressed`` checks original physical RX, including frames
        later dropped or rewritten by component hooks. The interval defaults
        to the ordinary CEC assertion timeout; directed Standby's separate
        250 ms physical completion assertion remains required by the scenario.
        """
        return _SourceIsolationObservation(self, source_component, upstream=upstream,
                                           since=since, duration=duration)

    def wait_for_switch_input(self, number, *, since, timeout=None):
        """Require a fresh, directed DUT Select AV Input Function and its effect.

        A numeric switch input is distinct from a CEC physical address. Local
        setters, virtual messages, stale packets and Set Stream Path do not
        prove that the requested explicit input command reached the switch.
        """
        if not self.can_route:
            raise ValueError("This component does not model HDMI input routing")
        if isinstance(number, bool) or not isinstance(number, int) or not 0 <= number <= 255:
            raise ValueError("Switch input must be an integer from 0 to 255")
        return self._wait_for_routing(number, since=since, timeout=timeout, command="select_av_input")

    def _wait_for_routing(self, expected, *, since, timeout, command, after=None, within=None):
        timing = self._response_timing(after, within)
        session, started, source, owner_scan = self._wire_context(since)
        switch = command == "select_av_input"
        field = "input" if switch else "physical_address"
        timeout = self._power_timeout if timeout is None else timeout
        if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or not math.isfinite(timeout) or timeout <= 0:
            raise ValueError("Routing assertion timeout must be a positive finite number")
        inspection_deadline = time.monotonic() + timeout
        deadline = inspection_deadline
        if timing:
            deadline = min(deadline, (timing["trigger_wire_timestamp_ns"] + timing["max_elapsed_ns"]) / 1_000_000_000)
        self._trace("component.routing.wait", command=command, **{field: expected},
                    since_monotonic_ns=started, initiator=source, timeout=timeout, **timing)
        requested = (f"Select AV Input Function for input {expected}" if switch else
                     f"{'Active Source' if command == 'active_source' else 'Set Stream Path'} for {expected:04x}")
        missing = f"{self.name} did not handle a fresh DUT {requested}"
        owner_checked = owner_scan is None
        owner_event = None
        while True:
            stopped = getattr(session, "_stop", None)
            if self._session is not session or stopped is not None and stopped.is_set():
                raise AssertionError("The CEC session ended before the requested input was confirmed")
            # Complete delivery to every peer (including the selected player)
            # before exposing the restored route to the scenario.
            # Timed assertions and a completed owner proof may inspect already
            # timely evidence after their wait budget. Inspection then cannot
            # block on the session lock or admit a changed/restored state.
            remaining = (inspection_deadline if timing else deadline) - time.monotonic()
            if (remaining <= 0 and not timing and owner_event is None) or not session._lock.acquire(timeout=max(0, remaining)):
                self._trace("component.routing.timeout", level=logging.ERROR,
                            command=command, **{field: expected}, since_monotonic_ns=started)
                raise TimeoutError(missing)
            try:
                session.assert_healthy()
                with self._power_condition:
                    revision = self._switch_input_revision if switch else self._input_revision
                    actual = self.selected_input if switch else self.active_input
                    matching = next((event for owner, event in reversed(self._routing_history)
                                     if owner is session and event["transport"] == "wire"
                                     and event["source"] == source and event["command"] == command
                                     and event["applied"] and event[field] == expected
                                     and event["wire_timestamp_ns"] >= started
                                     and event["processed_monotonic_ns"] >= started
                                     and event["state_revision"] == revision
                                     and self._response_in_time(event, timing)), None)
                    ready = (self.power != PowerState.OFF and actual == expected and matching is not None
                             and (not isinstance(self, PlaybackComponent) or self.active))
                    verified = ready and owner_checked and (owner_scan is None or revision == owner_event["state_revision"])
                    if not timing and time.monotonic() >= inspection_deadline and not (verified and owner_event is not None):
                        self._trace("component.routing.timeout", level=logging.ERROR,
                                    command=command, **{field: expected}, since_monotonic_ns=started)
                        raise TimeoutError(missing)
                    if verified:
                        if owner_event is not None:
                            matching = owner_event
                        self._routing_checks.append({"kind": "wire_command", field: expected, "command": command,
                                                     "timestamp_ns": time.monotonic_ns(), "since_monotonic_ns": started,
                                                     "frame": matching["frame"], "source": source,
                                                     "destination": matching["destination"],
                                                     "wire_timestamp_ns": matching["wire_timestamp_ns"],
                                                     **self._response_latency(matching, timing)})
                        self._trace("component.routing.assert_passed", kind="wire_command",
                                    command=command, **{field: expected}, frame=matching["frame"],
                                    source=source, destination=matching["destination"],
                                    since_monotonic_ns=started,
                                    wire_timestamp_ns=matching["wire_timestamp_ns"],
                                    **self._response_latency(matching, timing))
                        return self
                    recheck_owner = ready
                    if not ready:
                        owner_checked = owner_scan is None
                    remaining = deadline - time.monotonic()
                    if remaining <= 0 and not (timing and recheck_owner):
                        self._trace("component.routing.timeout", level=logging.ERROR,
                                    command=command, **{field: expected}, since_monotonic_ns=started)
                        raise TimeoutError(missing)
            finally:
                session._lock.release()
            if recheck_owner:
                self._wire_context(since)
                owner_event = matching
                owner_checked = True
                continue
            with self._power_condition:
                self._power_condition.wait(min(remaining, 0.05))

    def set_power(self, power):
        power = PowerState(power)
        # Serialize the entire power change with responder handling, including
        # the state update before the kernel's ACK mask is reconfigured.
        session = self._session
        with session._lock if session is not None else nullcontext():
            self._power_calls += 1
            previous = self._power
            if power == previous:
                return
            previous_revision = self._power_revision
            self._power = power
            self._power_revision += 1
            if power != PowerState.ON:
                self.pressed_key = None
            try:
                if session is not None:
                    session.power_changed(self, previous)
            except BaseException:
                self._power = previous
                self._power_revision = previous_revision
                raise
            self._trace("component.power.changed", before=previous.value, after=power.value,
                        state_revision=self._power_revision)

    def power_on(self):
        self.set_power(PowerState.ON)

    def standby(self):
        self.set_power(PowerState.STANDBY)

    def power_off(self):
        self.set_power(PowerState.OFF)

    def send(self, destination, opcode=None, operands=b""):
        if self._session is None:
            raise CecError("A component must be in an active CecSession to transmit")
        if self.power == PowerState.OFF:
            raise CecError("An unpowered component cannot transmit")
        frame = Frame(self.address.logical, destination, opcode, bytes(operands))
        if self.hooks.before_tx is not None:
            frame = self.hooks.before_tx(self, frame)
            if frame is None:
                return None
        if not isinstance(frame, Frame):
            raise TypeError("before_tx must return a Frame or None")
        if frame.source != self.address.logical:
            raise ValueError("A component may transmit only from its claimed logical address")
        result = self._session.transmit(self, frame)
        if self.hooks.after_tx is not None:
            self.hooks.after_tx(self, result)
        return result

    def activate(self):
        """Wake and claim Active Source; return the actual transmission result."""
        self._trace("component.activate", physical_address=self.address.physical)
        self.power_on()
        return self.send(15, 0x82, self.address.physical.to_bytes(2, "big"))

    def announce(self):
        if self.power == PowerState.OFF:
            return
        if self.cec_version >= 6:
            self.send(15, 0xa6, [self.cec_version, self.all_device_types, 0, 0])
        self.send(15, 0x84, self.address.physical.to_bytes(2, "big") + bytes([self.device_type]))
        self.send(15, 0x87, self.vendor_id.to_bytes(3, "big"))

    def receive(self, frame, *, transport="wire"):
        if transport not in ("wire", "virtual"):
            raise ValueError("CEC receive transport must be 'wire' or 'virtual'")
        # Negative wire assertions must see the original physical frame even
        # if destination filters or a fault hook suppress model handling.
        for observer in tuple(self._wire_observers):
            observer(self, frame, transport)
        if self.power == PowerState.OFF:
            return
        if frame.destination not in (self.address.logical, 15) or frame.source == self.address.logical:
            return
        if frame.timestamp_ns and frame.timestamp_ns < self._active_since_ns:
            return
        original = frame
        power_before = self.power
        if self.hooks.before_rx is not None:
            frame = self.hooks.before_rx(self, frame)
            if self.power == PowerState.OFF:
                return
            if frame is None:
                if self._session:
                    self._session._record(self, "rx", original, transport=transport, dropped=True)
                return
        if not isinstance(frame, Frame):
            raise TypeError("before_rx must return a Frame or None")
        if self._session:
            # Fault hooks may rewrite the handling payload or timestamp. Keep
            # the observed transport evidence intact and describe injected
            # handling separately so it cannot counterfeit a wire exchange.
            handling = ({"handled_frame": frame.raw.hex(":"),
                         "handled_timestamp_ns": frame.timestamp_ns}
                        if frame != original else {})
            self._session._record(self, "rx", original, transport=transport, **handling)
        power_calls = self._power_calls
        input_calls = self._input_calls
        switch_input_calls = self._switch_input_calls
        self.handle(frame)
        handled_power = self.power
        handled_revision = self._power_revision
        controlled_power = self._power_calls != power_calls
        handled_input, input_revision = self.active_input, self._input_revision
        controlled_input = self._input_calls != input_calls
        handled_switch_input, switch_input_revision = self.selected_input, self._switch_input_revision
        controlled_switch_input = self._switch_input_calls != switch_input_calls
        if self.hooks.after_rx is not None:
            self.hooks.after_rx(self, frame)
        self._retain_power_command(original, frame, transport, power_before,
                                   controlled_power, handled_power, handled_revision)
        self._retain_routing(original, frame, transport, controlled_input, handled_input, input_revision)
        self._retain_switch_input(original, frame, transport, controlled_switch_input,
                                  handled_switch_input, switch_input_revision)

    def _power_command(self, frame, before):
        """Recognize explicit commands supported by these component models."""
        if frame.source == 15:
            return None
        if frame.opcode == 0x36 and not frame.operands:
            return "standby", PowerState.STANDBY
        if frame.destination != self.address.logical:
            return None
        if isinstance(self, TvComponent) and frame.opcode in (0x04, 0x0d) and not frame.operands:
            return ("image_view_on" if frame.opcode == 0x04 else "text_view_on"), PowerState.ON
        if frame.opcode == 0x44 and len(frame.operands) == 1:
            key = frame.operands[0]
            if key == 0x6d:
                return "power_on_function", PowerState.ON
            if key == 0x6c:
                return "power_off_function", PowerState.STANDBY
            if key in (0x40, 0x6b):
                target = PowerState.STANDBY if before == PowerState.ON else PowerState.ON
                return "power_toggle", target
        return None

    def _retain_power_command(self, original, handled, transport, before,
                              controlled_power, handled_power, handled_revision):
        command = self._power_command(original, before)
        if command is None:
            return
        name, requested = command
        changed_payload = original.raw != handled.raw
        applied = (controlled_power and not changed_payload and handled_power == requested
                   and self.power == requested and handled_revision == self._power_revision)
        event = {"command": name, "requested_power": requested.value,
                 "frame": original.raw.hex(":"), "source": original.source,
                 "destination": original.destination, "transport": transport,
                 "wire_timestamp_ns": original.timestamp_ns if transport == "wire" else 0,
                 "processed_monotonic_ns": time.monotonic_ns(),
                 "before": before.value, "after": self.power.value,
                 "state_changed": before != self.power, "state_revision": self._power_revision,
                 "payload_modified": changed_payload, "applied": applied}
        with self._power_condition:
            self._power_history.append((self._session, event))
            self._power_condition.notify_all()

    def _retain_routing(self, original, handled, transport, controlled, physical, revision):
        source_request = (isinstance(self, PlaybackComponent) and original.opcode == 0x86
                          and len(original.operands) == 2
                          and int.from_bytes(original.operands, "big") == self.address.physical)
        if (not (self.can_route or source_request) or original.source == 15 or original.destination != 15
                or original.opcode not in (0x82, 0x86) or len(original.operands) != 2):
            return
        target = int.from_bytes(original.operands, "big")
        changed = original.raw != handled.raw
        event = {"opcode": original.opcode, "physical_address": target,
                 "command": "set_stream_path" if original.opcode == 0x86 else "active_source",
                 "frame": original.raw.hex(":"), "source": original.source,
                 "destination": original.destination, "transport": transport,
                 "wire_timestamp_ns": original.timestamp_ns if transport == "wire" else 0,
                 "processed_monotonic_ns": time.monotonic_ns(), "state_revision": self._input_revision,
                 "payload_modified": changed,
                 "applied": controlled and not changed and physical == target
                            and self.active_input == target and revision == self._input_revision
                            and (not source_request or self.active)}
        with self._power_condition:
            self._routing_history.append((self._session, event))
            self._power_condition.notify_all()

    def _retain_switch_input(self, original, handled, transport, controlled, number, revision):
        if (not self.can_route or original.source == 15 or original.destination != self.address.logical
                or original.opcode != 0x44 or len(original.operands) != 2 or original.operands[0] != 0x69):
            return
        target = original.operands[1]
        changed = original.raw != handled.raw
        event = {"opcode": original.opcode, "command": "select_av_input", "input": target,
                 "frame": original.raw.hex(":"), "source": original.source,
                 "destination": original.destination, "transport": transport,
                 "wire_timestamp_ns": original.timestamp_ns if transport == "wire" else 0,
                 "processed_monotonic_ns": time.monotonic_ns(), "state_revision": self._switch_input_revision,
                 "payload_modified": changed,
                 "applied": controlled and not changed and number == target
                            and self.selected_input == target and revision == self._switch_input_revision}
        with self._power_condition:
            self._routing_history.append((self._session, event))
            self._power_condition.notify_all()

    def handle(self, frame):
        """Handle standard discovery/power messages; override for extra behavior."""
        opcode, operands = frame.opcode, frame.operands
        if opcode is None or opcode == 0x00:
            return
        # Discovery requests are directed; malformed broadcast requests never
        # make every component answer an invalid query at the same time.
        if frame.destination != 15:
            if opcode == 0x83 and not operands:
                self.send(15, 0x84, self.address.physical.to_bytes(2, "big") + bytes([self.device_type]))
                return
            if opcode == 0x8c and not operands:
                self.send(15, 0x87, self.vendor_id.to_bytes(3, "big"))
                return
            if opcode == 0xa5 and not operands:
                if self.cec_version >= 6:
                    self.send(15, 0xa6, [self.cec_version, self.all_device_types, 0, 0])
                elif frame.source != 15:
                    self.send(frame.source, 0x00, [opcode, 0])
                return
            if frame.source != 15:
                if opcode == 0x46 and not operands:
                    self.send(frame.source, 0x47, self.name.encode("ascii"))
                    return
                if opcode == 0x9f and not operands:
                    self.send(frame.source, 0x9e, [self.cec_version])
                    return
                if opcode == 0x8f and not operands:
                    self.send(frame.source, 0x90, [0 if self.power == PowerState.ON else 1])
                    return
        if opcode == 0x36 and not operands:
            self.standby()
        elif (opcode == 0x44 and frame.destination != 15 and len(operands) == 2
              and operands[0] == 0x69 and self.can_route):
            self.pressed_key = operands[0]
            self.selected_input = operands[1]
        elif opcode == 0x44 and frame.destination != 15 and len(operands) == 1:
            self.pressed_key = operands[0]
            self.on_key(operands[0])
        elif opcode == 0x45 and frame.destination != 15 and not operands:
            self.pressed_key = None
        elif frame.destination != 15 and frame.source != 15:
            # Feature Abort: unrecognized opcode. Invalid operands on known
            # opcodes use reason 3 so test devices behave predictably.
            known = {0x83, 0x8c, 0xa5, 0x46, 0x9f, 0x8f, 0x36, 0x44, 0x45}
            self.send(frame.source, 0x00, [opcode, 3 if opcode in known else 0])

    def on_key(self, key):
        if key in (0x40, 0x6b):
            self.set_power(PowerState.STANDBY if self.power == PowerState.ON else PowerState.ON)
        elif key == 0x6d:
            self.power_on()
        elif key == 0x6c:
            self.standby()


class TvComponent(Component):
    device_type = 0
    logical_address_type = 0
    logical_addresses = (0,)
    all_device_types = 0x80
    can_route = True

    def __init__(self, attachment, name, *, direction=None, **kwargs):
        if not isinstance(attachment, (TentacleEndpoint, Component)) and direction is None:
            direction = "front"
        super().__init__(attachment, name, direction=direction, **kwargs)
        self.active_input = None
        self.volume = 25
        self.muted = False

    def wait_for_amity_input(self, *, since, timeout=None):
        """Prove Amity selected this display's inline HDMI input on its front wire.

        The downstream source can have a deeper physical address on another
        isolated wire. The display must receive Amity's own Active Source,
        apply its upstream input address, and still have that input selected.
        """
        if self.direction != "front":
            raise ValueError("Display input assertions need the adapter's front connection")
        return self._wait_for_routing(self.tv_input << 12, since=since, timeout=timeout,
                                     command="active_source")

    def handle(self, frame):
        if frame.opcode in (0x04, 0x0d) and frame.destination == self.address.logical and not frame.operands:
            self.power_on()
        elif frame.opcode in (0x82, 0x86) and frame.destination == 15 and len(frame.operands) == 2:
            self.active_input = int.from_bytes(frame.operands, "big")
        elif frame.opcode == 0x91 and frame.destination == self.address.logical and not frame.operands:
            self.send(15, 0x32, b"eng")
        else:
            super().handle(frame)

    def on_key(self, key):
        super().on_key(key)
        if key == 0x41:
            self.volume = min(100, self.volume + 1)
        elif key == 0x42:
            self.volume = max(0, self.volume - 1)
        elif key == 0x43:
            self.muted = not self.muted
        elif key in (0x65, 0x66):
            self.muted = key == 0x65


class AvrComponent(Component):
    device_type = 5
    logical_address_type = 4
    logical_addresses = (5,)
    all_device_types = 0x08
    can_route = True

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.volume = 25
        self.muted = False
        self.system_audio = False
        self.active_input = None

    def handle(self, frame):
        opcode, operands = frame.opcode, frame.operands
        if opcode == 0x70 and frame.destination == self.address.logical and len(operands) in (0, 2):
            self.system_audio = bool(operands)
            if self.system_audio:
                self.power_on()
                self.active_input = int.from_bytes(operands, "big")
            self.send(15, 0x72, [int(self.system_audio)])
        elif opcode == 0x7d and frame.destination == self.address.logical and not operands and frame.source != 15:
            self.send(frame.source, 0x7e, [int(self.system_audio)])
        elif opcode == 0x71 and frame.destination == self.address.logical and not operands and frame.source != 15:
            self.send(frame.source, 0x7a, [self.volume | (0x80 if self.muted else 0)])
        elif opcode in (0x82, 0x86) and frame.destination == 15 and len(operands) == 2:
            self.active_input = int.from_bytes(operands, "big")
        else:
            super().handle(frame)

    def on_key(self, key):
        super().on_key(key)
        if key == 0x41:
            self.volume = min(100, self.volume + 1)
        elif key == 0x42:
            self.volume = max(0, self.volume - 1)
        elif key == 0x43:
            self.muted = not self.muted
        elif key in (0x65, 0x66):
            self.muted = key == 0x65


class PlaybackComponent(Component):
    def __init__(self, *args, address_type="playback", **kwargs):
        # Allocation slots can differ while the model retains playback
        # identity and behavior. Linux forbids duplicate allocation types.
        profiles = {"playback": (3, (4, 8, 11)),
                    "recording": (1, (1, 2, 9)),
                    "tuner": (2, (3, 6, 7, 10))}
        if not isinstance(address_type, str) or address_type not in profiles:
            raise ValueError("Playback address type must be playback, recording or tuner")
        self.address_type = address_type
        self.logical_address_type, self.logical_addresses = profiles[address_type]
        super().__init__(*args, **kwargs)
        self.active = False
        self.playing = False

    def set_power(self, power):
        super().set_power(power)
        if self.power != PowerState.ON:
            self.active = self.playing = False
            self.active_input = None

    def wait_for_source_request(self, *, since, timeout=None):
        """Require a fresh physical Set Stream Path and this source's activation."""
        return self._wait_for_routing(self.address.physical, since=since, timeout=timeout,
                                     command="set_stream_path")

    def activate(self):
        self._trace("component.activate", physical_address=self.address.physical)
        self.power_on()
        self.active = True
        return self.send(15, 0x82, self.address.physical.to_bytes(2, "big"))

    def handle(self, frame):
        opcode, operands = frame.opcode, frame.operands
        if opcode == 0x85 and frame.destination == 15 and not operands:
            if self.active and self.power == PowerState.ON:
                self.send(15, 0x82, self.address.physical.to_bytes(2, "big"))
        elif opcode == 0x86 and frame.destination == 15 and len(operands) == 2:
            if int.from_bytes(operands, "big") == self.address.physical:
                self.active_input = self.address.physical
                self.activate()
        elif opcode == 0x82 and frame.destination == 15 and len(operands) == 2:
            if int.from_bytes(operands, "big") != self.address.physical:
                self.active = False
        elif opcode == 0x41 and frame.destination == self.address.logical and len(operands) == 1:
            self.power_on()
            self.playing = operands[0] != 0x25  # Play Still is a paused deck
        elif opcode == 0x42 and frame.destination == self.address.logical and len(operands) == 1:
            if operands[0] == 3:  # Stop
                self.playing = False
        elif opcode == 0x1a and frame.destination == self.address.logical and len(operands) == 1 and frame.source != 15:
            self.send(frame.source, 0x1b, [0x11 if self.playing else 0x1a])
        else:
            super().handle(frame)

    def on_key(self, key):
        super().on_key(key)
        if key in (0x44, 0x60):
            self.power_on()
            self.playing = True
        elif key in (0x45, 0x46, 0x61):
            self.playing = False
