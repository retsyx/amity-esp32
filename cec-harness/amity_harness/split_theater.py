"""Bounded required-source lookup evidence while another owner progresses."""

from copy import deepcopy
import threading
import time

from .cec import Frame, TxResult
from .components import PowerState
from .discovery import (_ObservedEquipment, _fresh_discovery, _native_selector,
                        _observed_matches, _identifier)
from .dut import _positive_seconds


class LookupNameGate:
    """Withhold only a source's OSD name, then answer its actual wire query.

    Enter before an explicit UI scan and bind that completed scan with
    ``set_missing_scan``. This leaves the device electrically present. Pending
    assertions must fit inside the firmware's ordinary bounded lookup scan;
    this helper never extends that scan or blocks the CEC responder thread.
    """

    def __init__(self, source, scan, *, progress):
        self.source, self.initial_scan = source, scan
        self.dut, self.session, self.bed = scan.dut, scan._session, scan._testbed
        self.progress = tuple(progress)
        if len(self.progress) != 2:
            raise ValueError("Lookup progress needs one TV and one receiver")
        tv, avr = self.progress
        if (tv.direction != "front" or tv.device_type != 0 or avr.direction != "back"
                or avr.device_type != 5 or tv.dut is not avr.dut or tv.tentacle != avr.tentacle
                or tv.dut is source.dut):
            raise ValueError("Lookup progress needs the other owner's front TV and back receiver")
        self._witnesses = (source, *self.progress)
        self._condition = threading.Condition()
        self._frames, self._progress_frames = [], []
        self._invalid_clock = self._entered = self._closed = self._open = False
        self._released = False
        self._released_at = None
        self.scan = None

    @property
    def released_at(self):
        """Successful physical OSD-name reply time, or None before that reply."""
        return self._released_at

    def _scope(self, scan, *, system=None):
        self.session.assert_healthy()
        source, bed = self.source, self.bed
        if (self.session._stop.is_set() or scan is not self.dut.last_scan
                or scan.dut is not self.dut or scan._session is not self.session
                or scan._testbed is not bed or bed.session is not self.session
                or not bed._entered or bed._closed or self.dut._testbed is not bed
                or source._session is not self.session or source.dut._testbed is not bed
                or source not in scan.expected or source.power == PowerState.OFF):
            raise AssertionError("Lookup name gate lost its current physical owner scan")
        if system is None:
            system = self.dut.get_system()
        source._validate_action_owners(time.monotonic_ns(), scan, self.session, _system=system)
        scan.validate_owners(self._witnesses, _system=system)
        for witness in self._witnesses:
            if (witness not in scan.expected or witness._session is not self.session
                    or witness.dut._testbed is not bed or witness.power == PowerState.OFF):
                raise AssertionError("Lookup progress changed its physical owner scope")
            initiator = scan.initiators.get(witness.gpio)
            occupied = {item.address.logical for item in self.session.components if item.gpio == witness.gpio}
            if type(initiator) is not int or not 0 <= initiator < 15 or initiator in occupied:
                raise AssertionError("Lookup name gate has no scan-proven physical DUT initiator")
        if self._entered:
            if (scan._boot != self._boot or scan._group != self._group
                    or scan.owner_boots != self._boots or scan.owner_duts != self._owners
                    or scan.initiators != self._initiators
                    or tuple((item.name, item.address, item.endpoint, item.gpio)
                             for item in self._witnesses) != self._identities):
                raise AssertionError("Lookup name gate changed source, owner boot or initiator")
            with self.session._lock:
                if (source.hooks is not self._hooks or source.hooks.before_tx is not self._before_tx
                        or any(getattr(source.hooks, name) is not None
                               for name in ("after_tx", "before_rx", "after_rx"))
                        or self._observe not in source._wire_observers):
                    raise AssertionError("Lookup name gate lost its original wire observation or hook")
                if any(self._observe_progress not in item._wire_observers for item in self.progress):
                    raise AssertionError("Lookup progress lost its original wire observation")

    def __enter__(self):
        if self._entered or self._closed:
            raise RuntimeError("A lookup name gate cannot be reused")
        self._scope(self.initial_scan)
        source, scan = self.source, self.initial_scan
        if source.direction != "back" or not _fresh_discovery(
                scan.records, source, scan.initiators[source.gpio], scan.started):
            raise AssertionError("Lookup name gate needs an initially discovered back-wire source")
        observed = _ObservedEquipment(self.dut, scan.status)
        matches = [observed.resolve(row) for row in scan.devices]
        matches = [item for item in matches if _observed_matches(item, source, source.endpoint)]
        if len(matches) != 1:
            raise AssertionError("Lookup name gate needs one initially observed source identity")
        self._name = deepcopy(matches[0][1])
        self._selectors = {matches[0][0]["name"], _native_selector(self._name)}
        self._boot, self._group = scan._boot, scan._group
        configuration = self.dut.request("/api/config")
        if (configuration.get("consortium_id") != self._group
                or not isinstance(configuration.get("components"), list)):
            raise AssertionError("Lookup name gate has no current saved equipment identity")
        for component in configuration["components"]:
            if self._name in component.get("qualified_names", ()):
                identity = component.get("component_id")
                if not _identifier(identity):
                    raise AssertionError("The gated source has a malformed saved selector")
                self._selectors.add("component:" + identity)
        self._boots, self._owners = dict(scan.owner_boots), dict(scan.owner_duts)
        self._initiators = dict(scan.initiators)
        self._identities = tuple((item.name, item.address, item.endpoint, item.gpio)
                                 for item in self._witnesses)
        self._hooks = source.hooks
        # Mutation hooks could counterfeit a query or reopen identity early.
        # Reject them rather than compose unverified fault injection.
        if any(getattr(self._hooks, name) is not None for name in
               ("before_tx", "after_tx", "before_rx", "after_rx")):
            raise ValueError("Lookup name gate requires a source without existing hooks")
        self._before_tx = self._filter
        with self.session._lock:
            self._started = time.monotonic_ns()
            source.hooks.before_tx = self._before_tx
            source._wire_observers.add(self._observe)
            for witness in self.progress:
                witness._wire_observers.add(self._observe_progress)
        self._entered = True
        self._report = {"kind": "lookup_name_gate", "component": source.name,
                        "owner": source.dut.identity.device_id, "gpio": source.gpio,
                        "started_monotonic_ns": self._started,
                        "owner_boots": dict(self._boots), "checks": []}
        if not hasattr(self.bed, "control_observations"):
            self.bed.control_observations = []
        self.bed.control_observations.append(self._report)
        return self

    def _filter(self, component, frame):
        # Called with the shared responder lock: never wait or perform HTTP.
        with self._condition:
            closed = not self._open
        return None if closed and frame.opcode == 0x47 else frame

    def _observe(self, component, frame, transport):
        if (transport != "wire" or frame.source != self._initiators[component.gpio]
                or frame.destination != component.address.logical
                or frame.opcode not in (0x83, 0x46) or frame.operands):
            return
        with self._condition:
            if type(frame.timestamp_ns) is not int or frame.timestamp_ns <= 0:
                self._invalid_clock = True
            elif frame.timestamp_ns >= self._started:
                if len(self._frames) < 4096:
                    self._frames.append(frame)
                else:
                    self._invalid_clock = True
            self._condition.notify_all()

    def _observe_progress(self, component, frame, transport):
        if (transport != "wire" or frame.source != self._initiators[component.gpio]
                or frame.opcode not in (0x04, 0x0d, 0x82, 0x44)):
            return
        with self._condition:
            if type(frame.timestamp_ns) is not int or frame.timestamp_ns <= 0:
                self._invalid_clock = True
            elif frame.timestamp_ns >= self._started:
                if len(self._progress_frames) < 4096:
                    self._progress_frames.append((component, frame, time.monotonic_ns()))
                else:
                    self._invalid_clock = True
            self._condition.notify_all()

    def _state_unresolved(self, state):
        if not isinstance(state.get("devices"), list):
            raise AssertionError("Lookup name gate has malformed current equipment evidence")
        if any(isinstance(row, dict) and row.get("name") in self._selectors
               for row in state["devices"]):
            raise AssertionError("The gated source name is still available in the equipment cache")

    def _unresolved(self, state, system):
        if (not isinstance(system.get("observations"), list)
                or system.get("octopus_id") != self.dut.identity.device_id
                or system.get("boot_id") != self._boot
                or system.get("consortium_id") != self._group
                or not isinstance(state.get("devices"), list)):
            raise AssertionError("Lookup name gate has malformed current equipment evidence")
        named = any(isinstance(row, dict) and row.get("resolved") is True
                    and row.get("stale") is False and row.get("qualified_name") == self._name
                    for row in system["observations"])
        self._state_unresolved(state)
        if named:
            raise AssertionError("The gated source name is still available in the equipment cache")

    def set_missing_scan(self, scan):
        """Bind the ordinary explicit scan that completed without this name."""
        if not self._entered or self._closed or self._open or self.scan is not None:
            raise RuntimeError("Bind one missing-name scan while the gate is closed")
        self._scope(scan)
        if (scan is self.initial_scan or scan.started < self._started
                or scan.status.get("discovering") is not False
                or type(scan.status.get("discovery_generation")) is not int):
            raise AssertionError("Lookup name gate needs a new completed explicit scan")
        self._revisions = {key: scan.status.get(key) for key in
                           ("expected_config_revision", "expected_presentation_revision")}
        if any(not isinstance(value, str) or not value for value in self._revisions.values()):
            raise AssertionError("Missing-name scan has no configuration revision evidence")
        self._generation = scan.status["discovery_generation"]
        state, system = self.dut.get_state(), self.dut.get_system()
        self._state_identity(state)
        if state.get("discovering") is not False:
            raise AssertionError("The missing-name scan has not completed")
        self._unresolved(state, system)
        self.scan = scan
        return self

    def _state_identity(self, state):
        if (state.get("expected_boot_id") != self._boot
                or state.get("discovery_generation") != self._generation
                or any(state.get(key) != value for key, value in self._revisions.items())):
            raise AssertionError("Lookup pending proof changed boot, configuration or completed scan generation")

    def _pending_state(self, state):
        # The coordinator can finish its own sequence while a member still
        # waits. Its settling flag cannot describe that member's paused job.
        return (state.get("discovering") is True
                and type(state.get("activity_settling")) is bool
                and (self.source.dut is not self.dut or state["activity_settling"]))

    def wait_for_progress(self, *, since, timeout=None):
        """Prove the other owner's commands before releasing a pending lookup.

        Physical B progress and A enumeration may arrive in either order.
        Read owner identity before waiting, then make just one state request
        after every required original frame has arrived. Applied command/state
        assertions remain the caller's responsibility after ``release``.
        """
        if not self.progress or not self._entered or self._closed or self._open or self.scan is None:
            raise RuntimeError("Lookup progress needs a closed gate and two progress components")
        started = getattr(since, "started_ns", None)
        if (type(started) is not int or started < self.scan.started
                or getattr(since, "kind", None) != "start"
                or getattr(since, "session", None) is not self.session):
            raise AssertionError("Lookup progress needs the original fresh activity start action")
        system = self.dut.get_system()
        self._scope(self.scan, system=system)
        self.source._validate_action_owners(since, self.scan, self.session, _system=system)
        timeout = self.dut._runtime.cec_timeout if timeout is None else timeout
        _positive_seconds(timeout, "Lookup progress timeout")
        deadline = time.monotonic() + timeout
        tv, avr = self.progress
        while True:
            self.session.assert_healthy()
            with self._condition:
                if self._invalid_clock:
                    raise AssertionError("Lookup progress lost original physical timestamp coverage")
                physical = [frame for frame in self._frames
                            if frame.timestamp_ns >= started and frame.opcode == 0x83]
                names = [frame for frame in self._frames if frame.timestamp_ns >= started
                         and frame.opcode == 0x46
                         and any(query.timestamp_ns <= frame.timestamp_ns for query in physical)]
                candidates = [(component, frame, received) for component, frame, received in self._progress_frames
                              if frame.timestamp_ns >= started]
                wake = [(frame, received) for component, frame, received in candidates if component is tv
                        and frame.destination == tv.address.logical and frame.opcode in (0x04, 0x0d)
                        and not frame.operands]
                route = [(frame, received) for component, frame, received in candidates if component is tv
                         and frame.destination == 15 and frame.opcode == 0x82
                         and frame.operands == (tv.tv_input << 12).to_bytes(2, "big")]
                power = [(frame, received) for component, frame, received in candidates if component is avr
                         and frame.destination == avr.address.logical and frame.opcode == 0x44
                         and frame.operands == b"\x6d"]
                if names and wake and route and power:
                    break
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("No complete fresh physical split-theater progress was observed")
                self._condition.wait(min(remaining, .02))
        # No owner HTTP reads follow these receipts. Keep the only pending-state
        # read causally after every B command and before the real name reply.
        requested = time.monotonic_ns()
        state = self.dut.get_state()
        self._state_identity(state)
        self._unresolved(state, system)
        self._scope(self.scan, system=system)
        if not self._pending_state(state):
            raise AssertionError("The source lookup ended before physical split-theater progress proof")
        frames = [(tv, *wake[-1]), (tv, *route[-1]), (avr, *power[-1])]
        self._report["checks"].append({"kind": "physical_progress", "passed": True,
            "since_monotonic_ns": started, "state_request_started_monotonic_ns": requested,
            "state_received_monotonic_ns": time.monotonic_ns(),
            "query_frame": names[-1].raw.hex(":"), "query_wire_timestamp_ns": names[-1].timestamp_ns,
            "discovering": True, "activity_settling": state["activity_settling"],
            "source_name_absent": True, "discovery_generation": self._generation, **self._revisions,
            "progress_frames": [{"component": component.name, "owner": component.dut.identity.device_id,
                                 "gpio": component.gpio, "frame": frame.raw.hex(":"),
                                 "wire_timestamp_ns": frame.timestamp_ns,
                                 "received_monotonic_ns": received} for component, frame, received in frames]})
        return self

    def release(self):
        """Open identity and physically answer the last actual OSD-name query."""
        if self._released:
            return self
        if not self._entered or self._closed:
            raise RuntimeError("Release an active lookup name gate")
        with self._condition:
            self._open = True
            queries = [frame for frame in self._frames if frame.opcode == 0x46]
        if queries:
            query = queries[-1]
            result = self.source.send(query.source, 0x47, self.source.name.encode("ascii"))
            expected = Frame(self.source.address.logical, query.source, 0x47,
                             self.source.name.encode("ascii"))
            if (not isinstance(result, TxResult) or not result.ok or result.transport != "wire"
                    or result.frame.raw != expected.raw or type(result.timestamp_ns) is not int
                    or result.timestamp_ns < query.timestamp_ns):
                raise AssertionError("Releasing the source name did not produce a successful physical OSD reply")
            self._released_at = result.timestamp_ns
            self._report["checks"].append({"kind": "release", "passed": True,
                "query_frame": query.raw.hex(":"), "query_wire_timestamp_ns": query.timestamp_ns,
                "reply_frame": result.frame.raw.hex(":"), "reply_wire_timestamp_ns": result.timestamp_ns})
        self._released = True
        return self

    def __exit__(self, exc_type, exc, traceback):
        try:
            self.release()
        except BaseException as error:
            if exc is None:
                raise
            exc.add_note("Lookup name gate release failed: " + type(error).__name__)
        finally:
            with self.session._lock:
                self.source._wire_observers.discard(self._observe)
                for witness in self.progress:
                    witness._wire_observers.discard(self._observe_progress)
                if self.source.hooks is self._hooks and self.source.hooks.before_tx is self._before_tx:
                    self.source.hooks.before_tx = None
            self._closed = True
        return False
