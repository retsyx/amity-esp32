"""Linux CEC wire access and lifetime management for dynamic Python fixtures.

The ioctl layout follows Linux's stable include/uapi/linux/cec.h ABI:
https://docs.kernel.org/userspace-api/media/cec/cec-header.html
The kernel handles signalling, ACKs and address claiming; Python handles device
responses in exclusive passthrough mode. No cec-follower process is used.
"""

from collections import deque
import ctypes
from dataclasses import dataclass
from contextvars import copy_context
import errno
import fcntl
import glob
import logging
import os
import select
import sys
import threading
import time

from .trace import event


class CecError(RuntimeError):
    pass


@dataclass(frozen=True)
class Frame:
    source: int
    destination: int
    opcode: int | None = None
    operands: bytes = b""
    timestamp_ns: int = 0

    def __post_init__(self):
        if not 0 <= self.source <= 15 or not 0 <= self.destination <= 15:
            raise ValueError("CEC source and destination must fit four bits")
        object.__setattr__(self, "operands", bytes(self.operands))
        if self.opcode is not None and not 0 <= self.opcode <= 255:
            raise ValueError("CEC opcode must fit one byte")
        if self.opcode is None and self.operands:
            raise ValueError("A CEC poll cannot have operands")
        if len(self.raw) > 16:
            raise ValueError("A CEC frame cannot exceed 16 bytes")

    @property
    def raw(self):
        header = bytes([(self.source << 4) | self.destination])
        return header if self.opcode is None else header + bytes([self.opcode]) + self.operands

    @classmethod
    def from_bytes(cls, raw, *, timestamp_ns=0):
        raw = bytes(raw)
        if not 1 <= len(raw) <= 16:
            raise ValueError("A CEC frame needs 1 through 16 bytes")
        return cls(raw[0] >> 4, raw[0] & 15, raw[1] if len(raw) > 1 else None,
                   raw[2:], timestamp_ns)


@dataclass(frozen=True)
class TxResult:
    frame: Frame
    status: int
    timestamp_ns: int = 0
    transport: str = "wire"

    @property
    def ok(self):
        return bool(self.status & 1)


class _Caps(ctypes.Structure):
    _fields_ = [("driver", ctypes.c_char * 32), ("name", ctypes.c_char * 32),
                ("available_log_addrs", ctypes.c_uint32), ("capabilities", ctypes.c_uint32),
                ("version", ctypes.c_uint32)]


class _LogAddrs(ctypes.Structure):
    _fields_ = [("log_addr", ctypes.c_uint8 * 4), ("log_addr_mask", ctypes.c_uint16),
                ("cec_version", ctypes.c_uint8), ("num_log_addrs", ctypes.c_uint8),
                ("vendor_id", ctypes.c_uint32), ("flags", ctypes.c_uint32),
                ("osd_name", ctypes.c_char * 15), ("primary_device_type", ctypes.c_uint8 * 4),
                ("log_addr_type", ctypes.c_uint8 * 4), ("all_device_types", ctypes.c_uint8 * 4),
                ("features", (ctypes.c_uint8 * 12) * 4)]


class _Message(ctypes.Structure):
    _fields_ = [("tx_ts", ctypes.c_uint64), ("rx_ts", ctypes.c_uint64),
                ("len", ctypes.c_uint32), ("timeout", ctypes.c_uint32),
                ("sequence", ctypes.c_uint32), ("flags", ctypes.c_uint32),
                ("msg", ctypes.c_uint8 * 16), ("reply", ctypes.c_uint8),
                ("rx_status", ctypes.c_uint8), ("tx_status", ctypes.c_uint8),
                ("tx_arb_lost_cnt", ctypes.c_uint8), ("tx_nack_cnt", ctypes.c_uint8),
                ("tx_low_drive_cnt", ctypes.c_uint8), ("tx_error_cnt", ctypes.c_uint8)]


def _request(direction, number, structure):
    # asm-generic/ioctl.h, used by the Raspberry Pi's aarch64 Linux ABI.
    return (direction << 30) | (ctypes.sizeof(structure) << 16) | (ord("a") << 8) | number


def _ioctl(fd, direction, number, value):
    buffer = bytearray(bytes(value))
    fcntl.ioctl(fd, _request(direction, number, type(value)), buffer, True)
    return type(value).from_buffer_copy(buffer)


class LinuxCecBackend:
    """Own one stable cec-gpio adapter, shared by components on that wire.

    Uses CEC 2.0 for its permitted one-address or TV/audio-system plus
    playback/recording configurations. Other supported combinations use CEC 1.4,
    up to the adapter's capacity and the four-address ioctl limit. Linux rejects
    duplicate allocation types and recording plus playback in either version.
    The selected version is exposed as ``cec_version`` after a successful claim.
    Kernel address allocation chooses addresses by type; the exact result is
    checked against the test's requested addresses. Closing clears all dynamic
    addresses and disables this adapter; it never reinstalls a static profile.
    """

    def __init__(self, gpio):
        self.gpio = gpio
        self.fd = None
        self.path = None
        self.cec_version = None

    def open(self):
        if sys.platform != "linux":
            raise CecError("Real CEC fixtures must run on the Linux Pi harness")
        if self.fd is not None:
            raise CecError("CEC adapter is already open")
        errors = []
        for path in sorted(glob.glob("/dev/cec*")):
            fd = None
            try:
                fd = os.open(path, os.O_RDWR | os.O_CLOEXEC)
                caps = _ioctl(fd, 3, 0, _Caps())
                if caps.driver.decode() != "cec-gpio" or caps.name.decode() != f"cec@{self.gpio}":
                    continue
                # This advisory lock protects against other framework processes;
                # exclusive kernel modes also fence cec-follower/cec-ctl users.
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                _ioctl(fd, 1, 9, ctypes.c_uint32(0x32))
                required = 0x0f  # userspace PA/LA, transmission and passthrough
                if caps.capabilities & required != required:
                    raise CecError(f"{path} lacks CEC configuration/transmit/passthrough support")
                self.fd, self.path, self.caps = fd, path, caps
                fd = None
                return self
            except (OSError, CecError) as exc:
                errors.append(f"{path}: {exc}")
            finally:
                if fd is not None:
                    os.close(fd)
        raise CecError(f"Cannot acquire cec-gpio cec@{self.gpio}; stop old follower services. "
                       + "; ".join(errors))

    def configure(self, components):
        if self.fd is None:
            raise CecError("CEC adapter is not open")
        components = list(components)
        if len(components) > min(self.caps.available_log_addrs, 4):
            raise CecError("This Linux CEC adapter cannot claim that many component addresses")
        types = [component.logical_address_type for component in components]
        if any(type(value) is not int or not 0 <= value <= 5 for value in types):
            raise CecError("Components need a registered Linux logical-address type")
        if len(set(types)) != len(types):
            raise CecError("Linux cannot emulate two components of the same logical-address type on one GPIO")
        if {1, 3} <= set(types):
            raise CecError("Linux cannot combine recording and playback allocation types on one GPIO")
        if any(type(c.device_type) is not int or c.device_type not in (0, 1, 3, 4, 5, 6, 7)
               for c in components):
            raise CecError("Invalid CEC primary device type")
        version = 6 if len(types) <= 1 or (len(types) == 2 and set(types) & {0, 4}
                                           and set(types) & {1, 3}) else 5
        _ioctl(self.fd, 3, 4, _LogAddrs())
        self.cec_version = None
        if not components:
            _ioctl(self.fd, 1, 2, ctypes.c_uint16(0xffff))
            return
        # Linux remaps the TV role to Specific Use at a nonzero adapter PA.
        # A TV+playback fixture therefore uses the TV's PA regardless of the
        # caller's component ordering; Python still replies with each own PA.
        physical = next((c.address.physical for c in components if c.device_type == 0),
                        components[0].address.physical)
        _ioctl(self.fd, 1, 2, ctypes.c_uint16(physical))
        addresses = _LogAddrs()
        addresses.cec_version = version
        addresses.num_log_addrs = len(components)
        addresses.vendor_id = 0xffffffff  # Python supplies each device's vendor
        addresses.osd_name = b"Amity fixture"
        for index, component in enumerate(components):
            addresses.primary_device_type[index] = component.device_type
            addresses.log_addr_type[index] = component.logical_address_type
            addresses.all_device_types[index] = component.all_device_types
            # RC profile 0 followed by device features 0, both end bits clear.
            addresses.features[index][0] = 0
            addresses.features[index][1] = 0
        actual = _ioctl(self.fd, 3, 4, addresses)
        wanted = [component.address.logical for component in components]
        claimed = list(actual.log_addr[:actual.num_log_addrs])
        if (actual.cec_version != version or claimed != wanted
                or actual.log_addr_mask != sum(1 << la for la in wanted)):
            _ioctl(self.fd, 3, 4, _LogAddrs())
            raise CecError(f"cec@{self.gpio} requested logical addresses {wanted}, claimed {claimed}; "
                           "a DUT/peer may occupy an address or the kernel chose a different type address")
        self.cec_version = version

    def transmit(self, frame):
        message = _Message()
        message.len = len(frame.raw)
        message.msg[:message.len] = frame.raw
        result = _ioctl(self.fd, 3, 5, message)
        return TxResult(frame, result.tx_status, result.tx_ts)

    def receive(self, timeout=0.05):
        ready, _, _ = select.select([self.fd], [], [], timeout)
        if not ready:
            return None
        message = _Message()
        message.timeout = 1
        try:
            result = _ioctl(self.fd, 3, 6, message)
        except OSError as exc:
            if exc.errno in (errno.ETIMEDOUT, errno.EAGAIN):
                return None
            raise
        if not result.rx_status & 1 or not result.len:
            return None
        return Frame.from_bytes(bytes(result.msg[:result.len]), timestamp_ns=result.rx_ts)

    def close(self):
        self.cec_version = None
        if self.fd is not None:
            fd, self.fd = self.fd, None
            errors = []
            for direction, number, value in ((3, 4, _LogAddrs()),
                                              (1, 2, ctypes.c_uint16(0xffff))):
                try:
                    _ioctl(fd, direction, number, value)
                except BaseException as exc:
                    errors.append(exc)
            try:
                os.close(fd)
            except BaseException as exc:
                errors.append(exc)
            if errors:
                raise errors[0]


class CecSession:
    """Context manager running per-test component responders in a background task.

    Components on an isolated GPIO wire also share a virtual bus. Directed
    traffic between those components stays in Python; broadcasts reach both
    virtual peers and the physical DUT. ``records`` labels each transport so
    internal exchanges cannot be mistaken for physical DUT traffic.
    Fault hooks operate above kernel signalling/ACK/address claiming. At startup
    the kernel announces its shared adapter physical address; Python follows
    with correct per-component announcements before the context becomes ready.
    """

    def __init__(self, components, *, backend_factory=LinuxCecBackend, max_records=10000):
        self.components = list(components)
        self.backend_factory = backend_factory
        if max_records < 1:
            raise ValueError("max_records must be positive")
        self._records = deque(maxlen=max_records)
        self._lock = threading.RLock()
        self._stop = threading.Event()
        self._backends = {}
        self._threads = []
        self._wire_idle_since_ns = {}
        self._error = None
        self._started = False
        self._virtual_depth = 0

    @property
    def records(self):
        with self._lock:
            return tuple(dict(record) for record in self._records)

    def _record(self, component, direction, frame, *, transport="wire", **extra):
        with self._lock:
            record = {"timestamp": time.monotonic(), "direction": direction,
                      "component": component.name, "gpio": component.gpio,
                      "frame": frame.raw.hex(":"), "source": frame.source,
                      "destination": frame.destination, "opcode": frame.opcode,
                      "transport": transport,
                      "wire_timestamp_ns": frame.timestamp_ns if transport == "wire" else 0,
                      **extra}
            self._records.append(record)
            # Emit every record independently of the bounded evidence ring.
            # Arbitrary backend exception text is deliberately excluded.
            event("cec.frame", level=logging.DEBUG,
                  dut=getattr(getattr(component, "dut", None), "name", None),
                  tentacle=getattr(component, "tentacle", None),
                  side=getattr(component, "direction", None),
                  **{key: record[key] for key in (
                      "timestamp", "direction", "component", "gpio", "frame", "source",
                      "destination", "opcode", "transport", "wire_timestamp_ns", "status",
                      "ok", "recipient", "recipients", "dropped", "handled_frame",
                      "handled_timestamp_ns") if key in record})

    def __enter__(self):
        from .topology import assign_addresses
        if self._started:
            raise CecError("A CEC session cannot be reused")
        self._started = True
        event("cec.session.start", components=[getattr(component, "name", None)
                                               for component in self.components])
        assign_addresses(self.components)
        try:
            for component in self.components:
                if component._session is not None:
                    raise CecError("Component already belongs to an active session")
                component._session = self
                component._active_since_ns = time.monotonic_ns()
                if component.gpio not in self._backends:
                    backend = self.backend_factory(component.gpio)
                    self._backends[component.gpio] = backend
                    backend.open()
                    event("cec.adapter.open", dut=getattr(getattr(component, "dut", None), "name", None),
                          gpio=component.gpio, tentacle=getattr(component, "tentacle", None),
                          side=getattr(component, "direction", None))
            for gpio in self._backends:
                self._configure(gpio)
            for gpio, backend in self._backends.items():
                # Retain the originating test label even if cleanup fails and
                # a delayed worker logs after the runner advances.
                thread = threading.Thread(target=copy_context().run,
                                          args=(self._run, gpio, backend), daemon=True,
                                          name=f"cec@{gpio}")
                self._threads.append(thread)
                thread.start()
            self.assert_healthy()
            event("cec.session.ready", wires=sorted(self._backends),
                  components=len(self.components))
            return self
        except BaseException as error:
            event("cec.session.failed", level=logging.ERROR, error_type=type(error).__name__)
            self.close()
            raise

    def _configure(self, gpio):
        active = [c for c in self.components if c.gpio == gpio and c.power.value != "off"]
        backend = self._backends[gpio]
        backend.configure(active)
        version = getattr(backend, "cec_version", None)
        version = version if type(version) is int and version in (5, 6) else 6
        for component in self.components:
            if component.gpio == gpio:
                component.cec_version = version
        event("cec.adapter.configured", gpio=gpio, cec_version=version,
              components=[{"dut": getattr(getattr(component, "dut", None), "name", None),
                           "component": getattr(component, "name", None),
                           "tentacle": getattr(component, "tentacle", None),
                           "side": getattr(component, "direction", None),
                           "logical_address": getattr(getattr(component, "address", None), "logical", None),
                           "physical_address": getattr(getattr(component, "address", None), "physical", None)}
                          for component in active])
        # Kernel-generated announcements carry the adapter's shared PA. Correct
        # each component's identity before beginning a scan or other scenario.
        for component in active:
            component.announce()

    def _run(self, gpio, backend):
        try:
            while not self._stop.is_set():
                polled_ns = time.monotonic_ns()
                frame = backend.receive(0.05)
                if frame is None:
                    with self._lock:
                        # Publish an idle poll's start only after it completes.
                        # A delayed SSH reply cannot prove that the remote
                        # receive FIFO was drained after a later boundary.
                        self._wire_idle_since_ns[gpio] = polled_ns
                    continue
                with self._lock:
                    for component in self.components:
                        if component.gpio == gpio:
                            component.receive(frame)
        except BaseException as exc:
            with self._lock:
                self._error = exc
                self._stop.set()
                event("cec.worker.failed", level=logging.ERROR, gpio=gpio,
                      error_type=type(exc).__name__)

    def assert_healthy(self):
        with self._lock:
            if self._error is not None:
                raise CecError("CEC responder failed") from self._error

    def _deliver_virtual(self, frame, recipients):
        """Synchronous delivery with normal RX hooks and bounded reply nesting."""
        if self._virtual_depth >= 32:
            raise CecError("Virtual CEC response loop exceeded 32 nested deliveries")
        self._virtual_depth += 1
        try:
            for peer in recipients:
                peer.receive(frame, transport="virtual")
        finally:
            self._virtual_depth -= 1

    def _transmit_virtual(self, component, frame, recipient):
        # Keep an Off component's address local: it must NACK rather than
        # accidentally sending the frame to a different physical recipient.
        accepted = recipient is not component and recipient.power.value != "off"
        now = time.monotonic_ns()
        result = TxResult(frame, 1 if accepted else 4, now, transport="virtual")
        self._record(component, "tx", frame, transport="virtual",
                     status=result.status, ok=result.ok, recipient=recipient.name)
        if accepted and frame.opcode is not None:
            received = Frame.from_bytes(frame.raw, timestamp_ns=now)
            self._deliver_virtual(received, [recipient])
        # Polls resolve presence without invoking a component opcode handler.
        return result

    def _broadcast_virtual(self, component, frame):
        recipients = [peer for peer in self.components
                      if peer is not component and peer.gpio == component.gpio
                      and peer.power.value != "off"]
        if recipients:
            self._record(component, "tx", frame, transport="virtual", status=1, ok=True,
                         recipients=tuple(peer.name for peer in recipients))
            received = Frame.from_bytes(frame.raw, timestamp_ns=time.monotonic_ns())
            self._deliver_virtual(received, recipients)

    def transmit(self, component, frame):
        with self._lock:
            self.assert_healthy()
            if self._stop.is_set():
                raise CecError("CEC session is stopping")
            if component not in self.components or component._session is not self:
                raise CecError("Component does not belong to this CEC session")
            if frame.source != component.address.logical:
                raise CecError("Component source does not match its logical address")
            if component.power.value == "off":
                raise CecError("An unpowered component cannot transmit")
            if frame.destination != 15:
                recipient = next((peer for peer in self.components
                                  if peer.gpio == component.gpio
                                  and peer.address.logical == frame.destination), None)
                if recipient is not None:
                    return self._transmit_virtual(component, frame, recipient)

            # External destinations and broadcasts still use the physical
            # adapter. Preserve wire ordering before peer replies, but make
            # internal broadcast delivery independent of wire success.
            try:
                result = self._backends[component.gpio].transmit(frame)
            except Exception as wire_error:
                event("cec.transmit.failed", level=logging.ERROR,
                      dut=getattr(getattr(component, "dut", None), "name", None), component=component.name,
                      gpio=component.gpio, tentacle=getattr(component, "tentacle", None),
                      side=getattr(component, "direction", None),
                      frame=frame.raw.hex(":"), error_type=type(wire_error).__name__)
                self._record(component, "tx", frame, status=0, ok=False,
                             error=f"{type(wire_error).__name__}: {wire_error}")
                if frame.destination == 15:
                    try:
                        self._broadcast_virtual(component, frame)
                    except Exception as virtual_error:
                        raise wire_error from virtual_error
                raise
            self._record(component, "tx", frame, wire_timestamp_ns=result.timestamp_ns,
                         status=result.status, ok=result.ok)
            if frame.destination == 15:
                self._broadcast_virtual(component, frame)
            return result

    def power_changed(self, component, previous):
        with self._lock:
            self.assert_healthy()
            if (previous.value == "off") != (component.power.value == "off"):
                views = [view for view in getattr(component, "_cec_views", (component,))
                         if view._session is self]
                for view in views:
                    view._active_since_ns = time.monotonic_ns()
                try:
                    for gpio in dict.fromkeys(view.gpio for view in views):
                        self._configure(gpio)
                except BaseException as exc:
                    # A failed reconfiguration may have cleared the old ACK
                    # mask. Fence the session instead of continuing with a
                    # Python power state that no longer matches the wire.
                    self._error = exc
                    self._stop.set()
                    event("cec.adapter.configure_failed", level=logging.ERROR,
                          gpio=component.gpio, component=component.name,
                          error_type=type(exc).__name__)
                    raise

    def close(self):
        event("cec.session.close", wires=sorted(self._backends))
        self._stop.set()
        for thread in self._threads:
            # Remote configuration/TX has a bounded 20-second RPC plus process
            # shutdown. Let admitted I/O finish before closing its descriptor.
            thread.join(30)
            if thread.is_alive():
                event("cec.worker.stop_failed", level=logging.ERROR, worker=thread.name)
                raise CecError("CEC worker did not stop; adapter is still in use")
        errors = []
        for gpio, backend in reversed(list(self._backends.items())):
            try:
                backend.close()
                event("cec.adapter.closed", gpio=gpio)
            except BaseException as exc:
                errors.append(exc)
                event("cec.adapter.close_failed", level=logging.ERROR,
                      gpio=gpio, error_type=type(exc).__name__)
        self._backends.clear()
        for component in self.components:
            if component._session is self:
                component._session = None
        if errors:
            raise CecError("Could not clear a dynamic CEC fixture") from errors[0]
        event("cec.session.closed")

    def __exit__(self, exc_type, exc, traceback):
        self.close()
        if exc is None:
            self.assert_healthy()
