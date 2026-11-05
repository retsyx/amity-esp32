"""Missing-equipment evidence through ordinary UI operations and physical CEC."""

from copy import deepcopy
import threading
import time

from .cec import Frame
from .discovery import _ObservedEquipment, _fresh_discovery, _observed_matches
from .trace import event


LOOKUP_COOLDOWN_SECONDS = 120


def assert_switch_commands(receiver, number, *, since, count):
    """Reject duplicated fallback work, or fallback when the source is known."""
    session, started, initiator, _ = receiver._wire_context(since)
    expected = Frame(initiator, receiver.address.logical, 0x44, bytes([0x69, number])).raw.hex(":")
    matches = [row for row in session.records if row.get("transport") == "wire"
               and row.get("direction") == "rx" and row.get("gpio") == receiver.gpio
               and row.get("component") == receiver.name and row.get("frame") == expected
               and row.get("wire_timestamp_ns", 0) >= started]
    if len(matches) != count:
        raise AssertionError("Activity emitted an unexpected number of numeric switch-input commands")
    receiver._routing_checks.append({"kind": "wire_command_count", "command": "select_av_input",
                                     "input": number, "frame": expected, "since_monotonic_ns": started,
                                     "count": count})


class LookupScans:
    """Observe lookup enumeration on known, independently isolated wires.

    Unannounced identity requests to still-present peers witness enumeration.
    A fresh physical announcement can also legitimately trigger targeted
    metadata queries; those require exact announcement/query correlation and
    unchanged UI discovery state. Original frames are observed before responder
    hooks. Nothing here asks the firmware to scan or changes its clock.
    """

    def __init__(self, scan, *witnesses):
        self.scan, self.witnesses = scan, tuple(witnesses)
        if not witnesses:
            raise ValueError("Lookup scan observation needs physical witnesses")
        self.session = scan._session
        self._condition = threading.Condition()
        self._frames = []
        self._invalid_clock = False
        self._closed = False

    def _validate(self):
        self.session.assert_healthy()
        if (self.session._stop.is_set() or self.scan.dut.last_scan is not self.scan
                or self.scan._session is not self.session
                or any(component._session is not self.session for component in self.witnesses)):
            raise AssertionError("Lookup observation lost its active CEC scan")
        self.scan.validate_owners(self.witnesses)

    def _validate_coverage(self):
        self._validate()
        with self.session._lock:
            if (not hasattr(self, "_started") or self._closed
                    or any(self._observe not in component._wire_observers for component in self.witnesses)):
                raise AssertionError("Lookup evidence needs an active registered wire observation")

    def __enter__(self):
        self._validate()
        self._started = time.monotonic_ns()
        # Start conservatively after the explicit scan completed: every
        # owner's real lookup timestamp is at least this old at expiry.
        self._expires = time.monotonic() + LOOKUP_COOLDOWN_SECONDS
        self._boot = self.scan._boot
        self._generation = self.scan.status.get("discovery_generation")
        with self.session._lock:
            for component in self.witnesses:
                component._wire_observers.add(self._observe)
        self._report = {"kind": "lookup_scans", "started_monotonic_ns": self._started,
                        "owner_boots": dict(self.scan.owner_boots),
                        "wires": [{"owner": component.dut.identity.device_id, "tentacle": component.tentacle,
                                   "side": component.direction, "gpio": component.gpio}
                                  for component in self.witnesses], "checks": []}
        bed = self.scan._testbed
        if not hasattr(bed, "control_observations"):
            bed.control_observations = []
        bed.control_observations.append(self._report)
        return self

    def _observe(self, component, frame, transport):
        if (transport != "wire" or frame.source != self.scan.initiators.get(component.gpio)
                or frame.destination != component.address.logical or frame.opcode not in (0x83, 0x46)):
            return
        with self._condition:
            if type(frame.timestamp_ns) is not int or frame.timestamp_ns <= 0:
                self._invalid_clock = True
            elif frame.timestamp_ns >= self._started:
                self._frames.append((component, frame))
            self._condition.notify_all()

    def _query_evidence(self):
        with self._condition:
            frames, invalid_clock = tuple(self._frames), self._invalid_clock
        records = self.session.records
        metadata, unexpected = [], []
        for component, frame in frames:
            proof = {"component": component.name, "owner": component.dut.identity.device_id,
                     "gpio": component.gpio, "initiator": self.scan.initiators[component.gpio],
                     "query_frame": frame.raw.hex(":"), "query_wire_timestamp_ns": frame.timestamp_ns}
            if frame.operands:
                unexpected.append(proof)
                continue
            expected = Frame(component.address.logical, 15, 0x84,
                             component.address.physical.to_bytes(2, "big")
                             + bytes([component.device_type])).raw.hex(":")
            earliest = max(self._started, frame.timestamp_ns - int(self.scan.dut._runtime.cec_timeout * 1e9))
            announcements = [row for row in records
                             if row.get("direction") == "tx" and row.get("transport") == "wire"
                             and row.get("gpio") == component.gpio and row.get("component") == component.name
                             and row.get("source") == component.address.logical and row.get("destination") == 15
                             and row.get("opcode") == 0x84 and row.get("frame") == expected
                             and row.get("ok") is True and not row.get("dropped")
                             and row.get("handled_frame", expected) == expected
                             and earliest <= row.get("wire_timestamp_ns", 0) < frame.timestamp_ns]
            if announcements:
                announcement = max(announcements, key=lambda row: row["wire_timestamp_ns"])
                # Both completions use the Pi's monotonic wire clock.
                metadata.append({**proof, "announcement_frame": expected,
                                 "announcement_wire_timestamp_ns": announcement["wire_timestamp_ns"]})
            else:
                unexpected.append(proof)
        return frames, invalid_clock, metadata, unexpected

    def _check_evidence(self, kind, through, *, state=None):
        frames, invalid_clock, metadata, unexpected = self._query_evidence()
        check = {"kind": kind, "through_monotonic_ns": through, "passed": False,
                 "invalid_clock": invalid_clock, "metadata_queries": metadata,
                 "unexpected_queries": unexpected,
                 "frames": [{"gpio": component.gpio, "frame": frame.raw.hex(":"),
                             "wire_timestamp_ns": frame.timestamp_ns} for component, frame in frames]}
        if state is not None:
            check.update(expected_boot=self._boot, observed_boot=state.get("expected_boot_id"),
                         expected_generation=self._generation, observed_generation=state.get("discovery_generation"),
                         discovering=state.get("discovering", False))
        self._report["checks"].append(check)
        return frames, check

    def _drain(self, through):
        deadline = time.monotonic() + self.scan.dut._runtime.cec_timeout
        while True:
            self.session.assert_healthy()
            if self.session._stop.is_set():
                raise AssertionError("CEC receive coverage ended during lookup observation")
            with self.session._lock:
                if all(self.session._wire_idle_since_ns.get(c.gpio, 0) >= through for c in self.witnesses):
                    return
            if time.monotonic() >= deadline:
                raise TimeoutError("Lookup observation did not drain physical CEC receive coverage")
            with self._condition:
                self._condition.wait(.02)

    def _no_scan_evidence(self, kind):
        self._validate_coverage()
        through = time.monotonic_ns()
        self._drain(through)
        state = self.scan.dut.get_state()
        self._validate_coverage()
        _, check = self._check_evidence(kind, through, state=state)
        if state.get("expected_boot_id") != self._boot:
            raise AssertionError("DUT restarted or began unsolicited discovery")
        if check["invalid_clock"] or check["unexpected_queries"]:
            raise AssertionError("An unannounced witness received unsolicited lookup identity queries")
        return through, state, check

    def assert_no_scan(self):
        through, state, check = self._no_scan_evidence("no_scan")
        if (state.get("discovering", False)
                or state.get("discovery_generation") != self._generation):
            raise AssertionError("DUT restarted or began unsolicited discovery")
        check["passed"] = True
        event("lookup.no_scan", dut=self.scan.dut.name, through_monotonic_ns=through)
        return self

    def assert_no_scan_on_witnesses(self):
        """Require no enumeration on these exact owner/adapter/side wires.

        The UI busy flag and generation aggregate other adapters, whose already
        requested scans may still finish independently. Retain those UI values
        as context; only physical witness evidence establishes scanning here.
        """
        through, _, check = self._no_scan_evidence("no_scan_on_witnesses")
        check["passed"] = True
        event("lookup.no_scan_on_witnesses", dut=self.scan.dut.name, through_monotonic_ns=through)
        return self

    def assert_passive_return(self, component, *, since):
        """Admit a returning peer using identity and scan proof on its own wire."""
        self._validate_coverage()
        if (type(since) is not int or since < self._started
                or not any(witness.dut is component.dut and witness.endpoint == component.endpoint
                           for witness in self.witnesses)):
            raise AssertionError("Passive return needs an observed witness on the same owner and CEC side")
        return _assert_returned(self.scan, component, since=since, observation=self)

    def wait_for_cooldown(self):
        """Require silence through expiry, including a window after expiry.

        Reaching the deadline alone must not schedule deferred enumeration.
        The next actual activity request is what may cause a scan.
        """
        while time.monotonic() < self._expires + .25:
            self.session.assert_healthy()
            with self._condition:
                interrupted = self._invalid_clock or bool(self._frames)
                if not interrupted:
                    self._condition.wait(min(.1, max(.001, self._expires + .25 - time.monotonic())))
            if interrupted:
                self._check_evidence("cooldown", time.monotonic_ns())
                raise AssertionError("A missing-device request deferred a scan until cooldown expiry")
        return self.assert_no_scan()

    def assert_scan_on(self, endpoint):
        self._validate()
        through = time.monotonic_ns()
        self._drain(through)
        frames, check = self._check_evidence("scan", through)
        if check["invalid_clock"]:
            raise AssertionError("Lookup scan evidence has no physical timestamp")
        if not frames or any(component.endpoint != endpoint for component, _ in frames):
            raise AssertionError("A lookup scan did not stay on the missing recipient's CEC side")
        witnesses = [component for component in self.witnesses if component.endpoint == endpoint]
        if not witnesses or not all(_fresh_discovery(self.session.records, component,
                                                    self.scan.initiators[component.gpio], self._started)
                                    for component in witnesses):
            raise AssertionError("Lookup scan has no fresh complete identity requests")
        state = self.scan.dut.get_state()
        if (state.get("expected_boot_id") != self._boot or state.get("discovering", False)
                or state.get("discovery_generation") == self._generation):
            raise AssertionError("Lookup scan did not complete on the original owner boot")
        catalog = _ObservedEquipment(self.scan.dut, state)
        resolved = [catalog.resolve(row) for row in state.get("devices", [])]
        for component in self.witnesses:
            if component.endpoint != endpoint and sum(_observed_matches(item, component, component.endpoint)
                                                      for item in resolved) != 1:
                raise AssertionError("A lookup scan erased the opposite side's equipment cache")
        self.scan._retain(state)
        check.update(passed=True, owner=endpoint.dut.identity.device_id,
                     tentacle=endpoint.tentacle, side=endpoint.direction)
        return self

    def __exit__(self, *args):
        with self.session._lock:
            for component in self.witnesses:
                component._wire_observers.discard(self._observe)
        self._closed = True


def _fresh_return(records, component, initiator, since, deadline):
    """A real PA announcement and fresh targeted OSD exchange on this wire.

    Reclaiming a Linux address can announce its shared adapter PA before the
    component's corrected PA. Metadata may legitimately be queried first;
    both exchanges must be fresh within this same return, and UI identity must
    agree with the corrected component announcement.
    """
    rows = [row for row in records if row.get("component") == component.name
            and row.get("gpio") == component.gpio and row.get("transport") == "wire"
            and since <= row.get("wire_timestamp_ns", 0) <= deadline and not row.get("dropped")
            and row.get("handled_frame", row.get("frame")) == row.get("frame")]
    announcement = Frame(component.address.logical, 15, 0x84,
                         component.address.physical.to_bytes(2, "big") + bytes([component.device_type])).raw.hex(":")
    query = Frame(initiator, component.address.logical, 0x46).raw.hex(":")
    reply = Frame(component.address.logical, initiator, 0x47, component.name.encode("ascii")).raw.hex(":")
    for announced in rows:
        if announced.get("direction") != "tx" or not announced.get("ok") or announced.get("frame") != announcement:
            continue
        for requested in rows:
            if requested.get("direction") != "rx" or requested.get("frame") != query:
                continue
            answer = next((answer for answer in rows if answer.get("direction") == "tx" and answer.get("ok")
                           and answer.get("frame") == reply
                           and answer["wire_timestamp_ns"] >= requested["wire_timestamp_ns"]), None)
            if answer is not None:
                return {"announcement_frame": announcement, "announcement_wire_timestamp_ns": announced["wire_timestamp_ns"],
                        "query_frame": query, "query_wire_timestamp_ns": requested["wire_timestamp_ns"],
                        "reply_frame": reply, "reply_wire_timestamp_ns": answer["wire_timestamp_ns"]}
    return None


def assert_returned(scan, component, *, since):
    """Prove passive return, then admit its evidence to this live scan proof.

    This issues reads only. The returned device must already be reported by
    the UI and have a new physical announcement/targeted identity exchange.
    Existing scan members, exact owner boots and wire initiators stay fenced.
    """
    return _assert_returned(scan, component, since=since)


def _assert_returned(scan, component, *, since, observation=None):
    session, dut = scan._session, scan.dut
    if (type(since) is not int or since <= 0 or component not in session.components
            or component._session is not session or dut.last_scan is not scan
            or scan.owner_duts.get(component.dut.identity.device_id) is not component.dut
            or component.gpio not in scan.initiators):
        raise AssertionError("Returned equipment has no authorized physical owner/wire proof")
    scan.validate_owners()
    deadline = since + int(dut._runtime.cec_timeout * 1e9)
    evidence = None

    def physical_evidence():
        proof = _fresh_return(session.records, component, scan.initiators[component.gpio], since, deadline)
        if proof is None and time.monotonic_ns() >= deadline:
            raise TimeoutError("Passive equipment return exceeded the physical CEC deadline")
        return proof

    # Owner and UI confirmation use bounded HTTP requests. Those reads may
    # finish after timely wire frames; they cannot extend the frames' window.
    physical_evidence()

    def returned(state):
        nonlocal evidence
        session.assert_healthy()
        if session._stop.is_set() or dut.last_scan is not scan:
            raise AssertionError("Returned equipment lost its CEC session")
        scan._observe(state)
        if observation is None and state.get("discovering", False):
            raise AssertionError("A passive equipment return launched a full scan")
        evidence = physical_evidence()
        catalog = _ObservedEquipment(dut, state)
        matches = [item for row in state.get("devices", [])
                   if _observed_matches((item := catalog.resolve(row)), component, component.endpoint)]
        return len(matches) == 1 and evidence is not None

    dut.wait_for(returned, timeout=dut._runtime.timeout, poll_interval=.05)
    scan.validate_owners()
    if observation is not None:
        observation.assert_no_scan_on_witnesses()
    if component not in scan.expected:
        scan.expected = (*scan.expected, component)
    scan._retain()
    proof = {"kind": "passive_return", "component": component.name,
             "owner": component.dut.identity.device_id, "gpio": component.gpio,
             "since_monotonic_ns": since, "deadline_monotonic_ns": deadline,
             "owner_boots": dict(scan.owner_boots), **evidence}
    bed = scan._testbed
    if not hasattr(bed, "control_observations"):
        bed.control_observations = []
    bed.control_observations.append(deepcopy(proof))
    event("equipment.returned", dut=dut.name, **proof)
    return scan
