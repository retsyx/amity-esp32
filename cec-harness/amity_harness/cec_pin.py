"""Decode original Linux CEC pin events, including a NACKed partial frame.

The pin-event ABI and timing limits follow Linux cec.h and cec-pin.c. Each
reported byte includes its actual EOM and ACK slots; no missing opcode or
operand is reconstructed when a transmitter stops at a NACKed header.
"""

from collections import deque
from dataclasses import dataclass
import errno
import math
import select
import threading
import time

from .cec import CecError, Frame, _Event, _ioctl


_PIN_REASONS = frozenset((
    "lost kernel messages", "dropped pin events", "regressing or invalid edge timestamp",
    "initial state after synchronization", "repeated pin level", "malformed start pulse",
    "repeated start rising edge", "malformed data pulse", "frame exceeds sixteen bytes",
    "malformed start period", "short terminal ACK slot", "malformed data period",
    "frame queue exceeded",
))
_PIN_NUMBERS = ("bit_index", "decoded_bytes", "synchronized_ns", "frame_start_ns",
                "last_event_ns", "fall_ns", "fault_ns", "edge_delta_ns", "low_delta_ns")


def validate_pin_diagnostic(value):
    """Copy only bounded, known pin data; never forward arbitrary error text."""
    fields = {"reason", "phase", "level", "previous_level", "terminal", "recent_events", *_PIN_NUMBERS}
    if not isinstance(value, dict) or set(value) != fields:
        raise ValueError("Unexpected CEC pin diagnostic fields")
    if (not isinstance(value["reason"], str) or value["reason"] not in _PIN_REASONS
            or value["phase"] not in ("seek", "start", "data")):
        raise ValueError("Unknown CEC pin diagnostic reason or phase")
    if any(type(value[name]) is not int or not -(2**64) < value[name] < 2**64 for name in _PIN_NUMBERS):
        raise ValueError("Invalid CEC pin diagnostic number")
    if not 0 <= value["bit_index"] <= 10 or not 0 <= value["decoded_bytes"] <= 16:
        raise ValueError("Invalid CEC pin diagnostic decoder position")
    if any(value[name] is not None and type(value[name]) is not bool for name in ("level", "previous_level")) or type(value["terminal"]) is not bool:
        raise ValueError("Invalid CEC pin diagnostic level")
    events = value["recent_events"]
    if not isinstance(events, (list, tuple)) or len(events) > 32:
        raise ValueError("CEC pin diagnostic history exceeded its bound")
    copied = []
    for row in events:
        if (not isinstance(row, (list, tuple)) or len(row) != 3
                or any(type(number) is not int for number in row)
                or not 0 <= row[0] <= 8 or not 0 <= row[1] < 2**64 or not 0 <= row[2] <= 2**32 - 1):
            raise ValueError("Invalid CEC pin diagnostic event")
        copied.append(list(row))
    return {**value, "recent_events": copied}


class PinCoverageError(CecError):
    def __init__(self, diagnostic):
        self.pin_diagnostic = validate_pin_diagnostic(diagnostic)
        super().__init__("Invalid CEC pin coverage: " + self.pin_diagnostic["reason"])


def pin_diagnostic(error):
    """Return a safe diagnostic only when the complete known schema validates."""
    value = getattr(error, "pin_diagnostic", None)
    if value is None:
        return None
    try:
        return validate_pin_diagnostic(value)
    except (TypeError, ValueError):
        return None


def pin_monitor_error(error):
    result = CecError("The CEC pin monitor lost physical coverage")
    diagnostic = pin_diagnostic(error)
    if diagnostic is not None:
        result.pin_diagnostic = diagnostic
    return result


@dataclass(frozen=True)
class PinFrame(Frame):
    eom: tuple[bool, ...] = ()
    ack: tuple[bool, ...] = ()
    synchronized_ns: int = 0

    def __post_init__(self):
        super().__post_init__()
        for name in ("eom", "ack"):
            values = tuple(getattr(self, name))
            if len(values) != len(self.raw) or any(type(value) is not bool for value in values):
                raise ValueError("Each observed CEC byte needs its original EOM and ACK bits")
            object.__setattr__(self, name, values)
        if (type(self.timestamp_ns) is not int or self.timestamp_ns <= 0
                or type(self.synchronized_ns) is not int
                or not 0 < self.synchronized_ns <= self.timestamp_ns):
            raise ValueError("A pin frame needs its actual start and synchronization timestamps")
        if any(self.eom[:-1]):
            raise ValueError("CEC bytes cannot follow an EOM slot")
        nack = (lambda value: not value) if self.destination == 15 else bool
        if any(nack(value) for value in self.ack[:-1]) or not (self.eom[-1] or nack(self.ack[-1])):
            raise ValueError("The observed frame must end at its actual EOM or NACK")

    @property
    def started_ns(self):
        return self.timestamp_ns

    @classmethod
    def observed(cls, raw, *, timestamp_ns, eom, ack, synchronized_ns):
        frame = Frame.from_bytes(raw, timestamp_ns=timestamp_ns)
        return cls(frame.source, frame.destination, frame.opcode, frame.operands,
                   frame.timestamp_ns, tuple(eom), tuple(ack), synchronized_ns)


class PinDecoder:
    """An edge-only decoder which locks at the first complete start pulse.

    Opening during an existing frame is permitted. Once synchronized, missing,
    malformed or regressing edges invalidate coverage instead of being skipped.
    Timing is judged only between actual pin events. An empty DQEVENT queue
    does not certify delivery from the kernel pin worker's upstream queue.
    """

    def __init__(self):
        self.synchronized_ns = 0
        self._last_ns = 0
        self._level = None
        self._fall = None
        self._phase = "seek"
        self._start = 0
        self._bits = 0
        self._byte = 0
        self._bytes, self._eom, self._ack = [], [], []
        self._terminal = False
        self._recent = deque(maxlen=32)
        self._fault_ns = self._event_delta_ns = 0
        self._previous_level = None

    @property
    def pending_start_ns(self):
        # An unclassified falling edge could be the next valid start.
        return self._start if self._phase != "seek" else (self._fall or 0)

    def _bad(self, message):
        raise PinCoverageError({"reason": message, "phase": self._phase,
            "bit_index": self._bits, "decoded_bytes": len(self._bytes), "level": self._level,
            "previous_level": self._previous_level, "terminal": self._terminal,
            "synchronized_ns": self.synchronized_ns, "frame_start_ns": self._start,
            "last_event_ns": self._last_ns, "fall_ns": self._fall or 0,
            "fault_ns": self._fault_ns, "edge_delta_ns": self._event_delta_ns,
            "low_delta_ns": self._fault_ns - self._fall if self._fall is not None else 0,
            "recent_events": tuple(self._recent)})

    def _seek(self):
        self._phase, self._start, self._fall = "seek", 0, None
        self._bits, self._byte = 0, 0
        self._bytes, self._eom, self._ack = [], [], []
        self._terminal = False

    def _emit(self):
        frame = PinFrame.observed(bytes(self._bytes), timestamp_ns=self._start,
                                  eom=self._eom, ack=self._ack,
                                  synchronized_ns=self.synchronized_ns)
        self._seek()
        return frame

    def feed(self, event, timestamp_ns, *, flags=0):
        # This history contains only kernel event kind/time/flags. It never
        # includes RPC payloads, credentials or arbitrary exception strings.
        if (type(event) is int and 0 <= event <= 8 and type(timestamp_ns) is int
                and 0 <= timestamp_ns < 2**64 and type(flags) is int and 0 <= flags < 2**32):
            self._recent.append((event, timestamp_ns, flags))
        self._fault_ns = timestamp_ns if type(timestamp_ns) is int and 0 <= timestamp_ns < 2**64 else 0
        self._event_delta_ns = self._fault_ns - self._last_ns
        self._previous_level = self._level
        if event == 2:
            self._bad("lost kernel messages")
        if event not in (3, 4):
            return ()
        if flags & ~3 or flags & 2:
            self._bad("dropped pin events")
        if type(timestamp_ns) is not int or timestamp_ns <= 0 or timestamp_ns <= self._last_ns:
            self._bad("regressing or invalid edge timestamp")
        self._last_ns = timestamp_ns
        high = event == 4
        if flags & 1:
            if self.synchronized_ns:
                self._bad("initial state after synchronization")
            self._level = high
            self._seek()
            return ()
        if self._level is high:
            if self.synchronized_ns:
                self._bad("repeated pin level")
            self._seek()
        self._level = high
        if high:
            if self._fall is None:
                return ()
            width = timestamp_ns - self._fall
            if self._phase == "seek":
                if 3_500_000 <= width <= 3_900_000:
                    self._phase, self._start = "start", self._fall
                elif self.synchronized_ns:
                    self._bad("malformed start pulse")
                else:
                    self._fall = None
                return ()
            if self._phase == "start":
                self._bad("repeated start rising edge")
            if 400_000 <= width <= 800_000:
                bit = True
            elif 1_300_000 <= width <= 1_700_000:
                bit = False
            else:
                self._bad("malformed data pulse")
            if self._bits < 8:
                self._byte = (self._byte << 1) | bit
            elif self._bits == 8:
                self._eom.append(bit)
            else:
                self._bytes.append(self._byte)
                self._ack.append(bit)
                broadcast = (self._bytes[0] & 15) == 15
                self._terminal = self._eom[-1] or (not bit if broadcast else bit)
                if len(self._bytes) == 16 and not self._terminal:
                    self._bad("frame exceeds sixteen bytes")
            self._bits += 1
            return ()
        result = ()
        if self._phase == "start":
            total = timestamp_ns - self._start
            if not 4_300_000 <= total <= 4_700_000:
                if self.synchronized_ns:
                    self._bad("malformed start period")
                self._seek()
            else:
                if not self.synchronized_ns:
                    self.synchronized_ns = self._start
                self._phase = "data"
        elif self._phase == "data":
            total = timestamp_ns - self._fall
            if self._terminal:
                if total < 2_050_000:
                    self._bad("short terminal ACK slot")
                result = (self._emit(),)
            elif not 2_050_000 <= total <= 2_750_000:
                self._bad("malformed data period")
            elif self._bits == 10:
                self._bits, self._byte = 0, 0
        self._fall = timestamp_ns
        return result



class PinMonitor:
    """Continuously drain a nonblocking monitor-pin FD independently of RPC.

    The worker must keep collecting edges while the responder's serialized RPC
    is configuring addresses or transmitting. Its bounded decoded-frame queue
    fails on overflow; a missing edge can never silently become an idle period.
    """

    def __init__(self, fd):
        self.fd = fd
        self.decoder = PinDecoder()
        self._condition = threading.Condition()
        self._frames = deque()
        self._error = None
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="cec-pin", daemon=True)
        self._thread.start()

    def _run(self):
        try:
            while not self._stop.is_set():
                select.select([], [], [self.fd], .001)
                frames = []
                while True:
                    try:
                        notice = _ioctl(self.fd, 3, 7, _Event())
                    except OSError as error:
                        if error.errno == errno.EAGAIN:
                            break
                        raise
                    frames.extend(self.decoder.feed(notice.event, notice.timestamp_ns, flags=notice.flags))
                with self._condition:
                    if len(self._frames) + len(frames) > 4096:
                        self.decoder._bad("frame queue exceeded")
                    self._frames.extend(frames)
                    if frames:
                        self._condition.notify_all()
        except BaseException as error:
            with self._condition:
                self._error = error
                self._condition.notify_all()

    def receive(self, timeout):
        if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or not math.isfinite(timeout) or not 0 <= timeout <= 1:
            raise ValueError("CEC monitor timeout must be between zero and one second")
        deadline = time.monotonic() + timeout
        with self._condition:
            while True:
                if self._error is not None:
                    raise pin_monitor_error(self._error) from self._error
                if self._stop.is_set():
                    raise CecError("The CEC pin monitor is closed")
                if self._frames:
                    return self._frames.popleft()
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return None
                self._condition.wait(remaining)

    def close(self):
        self._stop.set()
        with self._condition:
            self._condition.notify_all()
        self._thread.join(timeout=2)
        if self._thread.is_alive():
            raise TimeoutError("The CEC pin reader did not stop")
        if self._error is not None:
            raise pin_monitor_error(self._error) from self._error
