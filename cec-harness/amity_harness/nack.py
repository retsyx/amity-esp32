"""A real absent CEC recipient, with independent physical-wire observation."""

import threading
import time
from contextvars import copy_context

from .cec import CecError
from .cec_pin import PinFrame, pin_diagnostic, validate_pin_diagnostic
from .components import PowerState
from .trace import event


class MissingCECRecipient:
    """Withdraw one fixture address without hiding its original wire traffic.

    The ordinary responder keeps its exclusive descriptor and the other peer's
    address. A separate pin-monitor descriptor captures actual bus edges. An
    unclaimed recipient therefore really NACKs; model RX hooks cannot do that.
    The sender may stop at the header, so observations retain partial messages
    rather than inventing a key opcode or operand that never reached the wire.
    """

    def __init__(self, component, *, scan):
        self.component, self.scan = component, scan
        self.session = scan._session
        self._condition = threading.Condition()
        self._frames = []
        self._stop = threading.Event()
        self._thread = self._backend = None
        self._error = None
        self._entered = self._closed = self._restored = False
        self.coverage_started_ns = 0
        self._pin_diagnostic = None
        self._drain_witnesses = []

    def _validate(self):
        component, scan, session = self.component, self.scan, self.session
        bed = getattr(scan, "_testbed", None)
        if (bed is None or not bed._entered or bed._closed or bed.session is not session
                or scan.dut.last_scan is not scan or component not in scan.expected
                or component not in session.components or component._session is not session
                or component.dut._testbed is not bed):
            raise AssertionError("A missing recipient needs the current physical scan and TestBed")
        scan.validate_owners((component,))
        component._wire_context(time.monotonic_ns())
        session.assert_healthy()

    def __enter__(self):
        if self._entered or self._closed:
            raise RuntimeError("A missing-recipient observation cannot be reused")
        self._validate()
        component = self.component
        if component.power == PowerState.OFF:
            raise ValueError("Withdraw a currently powered, scanned recipient")
        peers = [peer for peer in self.session.components
                 if peer is not component and peer.gpio == component.gpio and peer.power != PowerState.OFF]
        if not peers:
            raise ValueError("Keep another fixture address active on the observed CEC wire")
        initiator = self.scan.initiators.get(component.gpio)
        if (type(initiator) is not int or not 0 <= initiator < 15
                or any(peer.gpio == component.gpio and peer.address.logical == initiator
                       for peer in self.session.components)):
            raise ValueError("The physical drain witness needs an unclaimed DUT initiator address")
        self._witness = peers[0]
        self._power = component.power
        self._owner_boots, self._initiators = dict(self.scan.owner_boots), dict(self.scan.initiators)
        self._backend = self.session._backends[component.gpio]
        self._backend.open_monitor()
        self.started_ns = time.monotonic_ns()
        bed = self.scan._testbed
        if not hasattr(bed, "control_observations"):
            bed.control_observations = []
        bed.control_observations.append({"kind": "cec_pin_drain", "component": component.name,
            "owner": component.dut.identity.device_id, "gpio": component.gpio,
            "started_monotonic_ns": self.started_ns, "checks": self._drain_witnesses})
        self._entered = True
        self._thread = threading.Thread(target=copy_context().run, args=(self._receive,),
                                        name="cec-nack@" + str(component.gpio), daemon=True)
        self._thread.start()
        try:
            component.power_off()
            self.withdrawn_ns = time.monotonic_ns()
            self.assert_healthy()
        except BaseException as error:
            self._note_pin_diagnostic(error, "withdraw")
            try:
                self.close()
            except BaseException as cleanup_error:
                self._note_pin_diagnostic(cleanup_error, "withdraw_cleanup")
                if hasattr(error, "add_note"):
                    error.add_note("CEC recipient cleanup failed: " + type(cleanup_error).__name__)
            raise
        event("cec.recipient.withdrawn", component=component.name, gpio=component.gpio,
              owner=component.dut.identity.device_id, logical_address=component.address.logical,
              since_monotonic_ns=self.withdrawn_ns)
        return self

    def _receive(self):
        try:
            while not self._stop.is_set():
                frame = self._backend.receive_monitor(.02)
                with self._condition:
                    if frame is not None:
                        if not isinstance(frame, PinFrame):
                            raise AssertionError("A failed-key observation needs decoded physical pin evidence")
                        if self.coverage_started_ns and self.coverage_started_ns != frame.synchronized_ns:
                            raise AssertionError("The pin monitor changed its original synchronization boundary")
                        self.coverage_started_ns = frame.synchronized_ns
                        if len(self._frames) >= 4096:
                            raise AssertionError("The failed-key wire observation exceeded its frame bound")
                        if frame.timestamp_ns >= self.started_ns:
                            self._frames.append(frame)
                    self._condition.notify_all()
        except BaseException as error:
            self._note_pin_diagnostic(error, "monitor_receive")
            with self._condition:
                self._error = error
                self._condition.notify_all()

    def _note_pin_diagnostic(self, error, stage):
        diagnostic = pin_diagnostic(error)
        if diagnostic is None and self._backend is not None:
            value = getattr(self._backend, "last_pin_diagnostic", None)
            if value is not None:
                try:
                    diagnostic = validate_pin_diagnostic(value)
                except (TypeError, ValueError):
                    pass
        if diagnostic is None:
            return
        error.pin_diagnostic = diagnostic
        with self._condition:
            first = self._pin_diagnostic is None
            if first:
                self._pin_diagnostic = diagnostic
        if first:
            event("cec.pin.coverage_failed", component=self.component.name, gpio=self.component.gpio,
                  owner=self.component.dut.identity.device_id, stage=stage, diagnostic=diagnostic)

    def assert_healthy(self):
        if not self._entered or self._closed or self._stop.is_set():
            raise AssertionError("The failed-key wire observation is not active")
        self.session.assert_healthy()
        if (self.session._stop.is_set() or self.component._session is not self.session
                or self.scan.dut.last_scan is not self.scan
                or self.scan.owner_boots != self._owner_boots or self.scan.initiators != self._initiators):
            raise AssertionError("The failed-key observation lost its original owner or wire")
        with self._condition:
            if self._error is not None:
                error = CecError("Failed-key wire observation failed")
                if self._pin_diagnostic is not None:
                    error.pin_diagnostic = validate_pin_diagnostic(self._pin_diagnostic)
                raise error from self._error
        return self

    @property
    def frames(self):
        self.assert_healthy()
        with self._condition:
            return tuple(self._frames)

    def drain(self, through):
        if type(through) is not int or through < self.started_ns:
            raise ValueError("Drain an active failed-key observation boundary")
        deadline = max(time.monotonic(), through / 1e9) + self.scan.dut._runtime.cec_timeout
        uncertainty = getattr(self._backend, "clock_uncertainty_ns", 0)
        if type(uncertainty) is not int or uncertainty < 0:
            raise AssertionError("The failed-key monitor needs a bounded physical wire clock")
        submit_after = through + 2 * uncertainty
        while True:
            self.assert_healthy()
            now = time.monotonic_ns()
            if now >= submit_after:
                break
            remaining = min(deadline - time.monotonic(), (submit_after - now) / 1e9)
            if remaining <= 0:
                raise TimeoutError("Failed-key physical drain witness deadline expired")
            with self._condition:
                self._condition.wait(min(remaining, .02))

        witness = self._witness
        initiator = self._initiators[self.component.gpio]
        expected = bytes([(witness.address.logical << 4) | initiator, 0x9f])
        if witness._session is not self.session or witness.power == PowerState.OFF:
            raise AssertionError("The same-wire physical drain witness is no longer active")
        # Both are ordinary physical identity queries. The second start closes
        # the first terminal ACK slot even when the DUT sends no application
        # reply; empty event reads and host time cannot certify pin coverage.
        for _ in range(2):
            self.assert_healthy()
            if time.monotonic() >= deadline:
                raise TimeoutError("Failed-key physical drain witness deadline expired")
            result = witness.send(initiator, 0x9f)
            if (getattr(result, "transport", None) != "wire" or getattr(result, "ok", None) is not True
                    or getattr(getattr(result, "frame", None), "raw", None) != expected):
                raise AssertionError("The drain witness did not send its exact physical CEC query")

        while True:
            self.assert_healthy()
            with self._condition:
                matches = [frame for frame in self._frames
                           if frame.raw == expected and frame.timestamp_ns >= through
                           and frame.eom == (False, True) and frame.ack == (False, False)]
                if matches:
                    frame = min(matches, key=lambda candidate: candidate.timestamp_ns)
                    if (not 0 < self.coverage_started_ns
                            or self.coverage_started_ns + 2 * uncertainty > self.withdrawn_ns):
                        raise AssertionError("Pin coverage must synchronize before recipient withdrawal finishes")
                    record = {"kind": "physical_drain_witness", "requested_boundary_monotonic_ns": through,
                        "witness_component": witness.name, "frame": frame.raw.hex(":"),
                        "wire_timestamp_ns": frame.timestamp_ns, "source": frame.source,
                        "destination": frame.destination, "eom": list(frame.eom), "ack": list(frame.ack),
                        "synchronized_ns": frame.synchronized_ns}
                    self._drain_witnesses.append(record)
                    event("cec.pin.drain_witness", component=self.component.name, gpio=self.component.gpio,
                          owner=self.component.dut.identity.device_id, **record)
                    return frame.timestamp_ns
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("No exact physical CEC drain witness reached the pin monitor")
                self._condition.wait(min(remaining, .02))

    def restore(self):
        self.assert_healthy()
        if not self._restored:
            self.component.set_power(self._power)
            self._restored = True
            self.restored_ns = time.monotonic_ns()
        return self.restored_ns

    def close(self):
        if self._closed:
            return
        errors = []
        if self._entered and not self._restored:
            try:
                self.component.set_power(self._power)
                self._restored = True
            except BaseException as error:
                self._note_pin_diagnostic(error, "restore_cleanup")
                errors.append(error)
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2)
            if self._thread.is_alive():
                errors.append(TimeoutError("Failed-key wire monitor did not stop"))
        if self._backend is not None:
            try:
                self._backend.close_monitor()
            except BaseException as error:
                self._note_pin_diagnostic(error, "monitor_close")
                errors.append(error)
        self._closed = True
        if errors:
            raise errors[0]

    def __exit__(self, exc_type, exc, traceback):
        try:
            self.close()
        except Exception as error:
            if exc is None:
                raise
            if hasattr(exc, "add_note"):
                exc.add_note("CEC recipient restoration failed: " + type(error).__name__)
        return False
