"""Positive CEC start-edge proof during real BLE activity transitions."""

from contextvars import copy_context
import threading
import time

from .cec import CecError
from .cec_pin import PinFrame, pin_monitor_error
from .dut import _positive_seconds


class PinObservation:
    """Observe one scanned wire without changing its responders or power."""

    def __init__(self, component, *, scan):
        self.component, self.scan, self.session = component, scan, scan._session
        self.bed = scan._testbed
        self._condition, self._stop = threading.Condition(), threading.Event()
        self._frames, self._error = [], None
        self._thread = self._backend = None
        self._entered = self._closed = False
        self._synchronized_ns = 0

    def _validate(self):
        component, scan, session, bed = self.component, self.scan, self.session, self.bed
        if (not bed._entered or bed._closed or bed.session is not session or session._stop.is_set()
                or scan.dut.last_scan is not scan or scan._session is not session or scan._testbed is not bed
                or component not in scan.expected or component not in session.components
                or component._session is not session or component.dut._testbed is not bed):
            raise AssertionError("Pin observation needs the current physical scan and TestBed")
        session.assert_healthy()
        scan.validate_owners((component,))
        component._wire_context(time.monotonic_ns())
        if self._entered:
            self.assert_healthy()

    def __enter__(self):
        if self._entered or self._closed:
            raise RuntimeError("A physical pin observation cannot be reused")
        self._validate()
        self._owner_boots, self._initiators = dict(self.scan.owner_boots), dict(self.scan.initiators)
        self._backend = self.session._backends[self.component.gpio]
        self.clock_uncertainty_ns = getattr(self._backend, "clock_uncertainty_ns", 0)
        if type(self.clock_uncertainty_ns) is not int or self.clock_uncertainty_ns < 0:
            raise AssertionError("Pin observation needs a bounded physical wire clock")
        self._backend.open_monitor()
        self.started_ns = time.monotonic_ns()
        self._entered = True
        try:
            self._thread = threading.Thread(target=copy_context().run, args=(self._receive,),
                                            name="cec-pin-observe@" + str(self.component.gpio), daemon=True)
            self._thread.start()
        except BaseException as error:
            try:
                self._backend.close_monitor()
            except BaseException as cleanup_error:
                if hasattr(error, "add_note"):
                    error.add_note("Positive pin monitor cleanup failed: " + type(cleanup_error).__name__)
            self._closed = True
            raise
        return self

    def _receive(self):
        try:
            while not self._stop.is_set():
                frame = self._backend.receive_monitor(.02)
                if frame is None:
                    continue
                with self._condition:
                    if not isinstance(frame, PinFrame):
                        raise AssertionError("Pin observation requires original decoded physical edges")
                    if self._synchronized_ns and frame.synchronized_ns != self._synchronized_ns:
                        raise AssertionError("Pin observation changed its synchronization boundary")
                    self._synchronized_ns = frame.synchronized_ns
                    if len(self._frames) >= 4096:
                        raise AssertionError("The positive pin observation exceeded its frame bound")
                    if frame.timestamp_ns >= self.started_ns:
                        self._frames.append(frame)
                    self._condition.notify_all()
        except BaseException as error:
            with self._condition:
                self._error = error
                self._condition.notify_all()

    def assert_healthy(self):
        # Keep the input burst independent of model locks and HTTP latency.
        if (not self._entered or self._closed or self._stop.is_set() or self.session._stop.is_set()
                or self.bed._closed or not self.bed._entered or self.bed.session is not self.session
                or self.component._session is not self.session or self.component.dut._testbed is not self.bed
                or self.scan.dut.last_scan is not self.scan or self.scan._session is not self.session
                or self.scan._testbed is not self.bed
                or self.scan.owner_boots != self._owner_boots or self.scan.initiators != self._initiators
                or self.session._backends.get(self.component.gpio) is not self._backend
                or getattr(self._backend, "clock_uncertainty_ns", 0) != self.clock_uncertainty_ns):
            raise AssertionError("The positive pin observation lost its active scan, owner or wire")
        with self._condition:
            if self._error is not None:
                raise pin_monitor_error(self._error) from self._error
        return self

    def first_frame(self, raw, *, after, timeout=None):
        self.assert_healthy()
        if not isinstance(raw, bytes) or not 1 <= len(raw) <= 16:
            raise ValueError("Match an exact physical CEC frame of one to sixteen bytes")
        if type(after) is not int or after < self.started_ns:
            raise ValueError("Match a fresh frame after this pin observation began")
        timeout = self.scan.dut._runtime.cec_timeout if timeout is None else timeout
        _positive_seconds(timeout, "Positive pin observation timeout")
        deadline = time.monotonic() + timeout
        while True:
            self.assert_healthy()
            with self._condition:
                matches = [frame for frame in self._frames if frame.raw == raw and frame.timestamp_ns > after
                           and frame.eom[-1] is True and all(value == ((raw[0] & 15) == 15) for value in frame.ack)]
                if matches:
                    return min(matches, key=lambda frame: frame.timestamp_ns)
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("No exact fresh physical CEC start reached the pin observation")
                self._condition.wait(min(remaining, .02))

    def __exit__(self, exc_type, exc, traceback):
        errors = []
        if exc is None:
            try:
                self._validate()
            except BaseException as error:
                errors.append(error)
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2)
            if self._thread.is_alive():
                errors.append(TimeoutError("The positive pin observation did not stop"))
        try:
            self._backend.close_monitor()
        except BaseException as error:
            errors.append(error)
        self._closed = True
        if errors and exc is None:
            raise errors[0]
        if errors and exc is not None and hasattr(exc, "add_note"):
            exc.add_note("Positive pin observation cleanup failed: " + type(errors[0]).__name__)
        return False
