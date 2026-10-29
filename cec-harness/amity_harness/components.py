"""Stateful HDMI-CEC components, configured by each functional test."""

from dataclasses import dataclass
from contextlib import nullcontext
from enum import Enum

from .cec import CecError, Frame
from .topology import CecAddress


class PowerState(str, Enum):
    ON = "on"
    STANDBY = "standby"
    OFF = "off"


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


class Component:
    """Generic registered CEC device with identity, power and message hooks.

    ``parent`` and ``port`` describe HDMI topology, independent of the DUT wire
    attachment. ``address`` is a CecAddress(logical, physical), or None for
    assign_addresses(). Generic devices default to playback address semantics;
    subclasses can override device_type/logical_address_type/logical_addresses.
    Only powered On/Standby devices claim a logical address or receive frames.
    """

    device_type = 4
    logical_address_type = 3
    logical_addresses = (4, 8, 11)
    all_device_types = 0x10
    can_route = False

    def __init__(self, dut, name, *, tentacle=0, direction="back", address=None,
                 parent=None, port=None, power=PowerState.STANDBY, vendor_id=1,
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
        self.dut, self.name, self.tentacle, self.direction = dut, name, tentacle, direction
        self.gpio = dut.wiring.cec_gpio(dut.name, tentacle, direction)
        self.address, self.parent, self.port = address, parent, port
        self.vendor_id, self.hooks = vendor_id, hooks or Hooks()
        self._power = PowerState(power)
        self._session = None
        self._active_since_ns = 0
        self.pressed_key = None

    @property
    def power(self):
        return self._power

    def set_power(self, power):
        power = PowerState(power)
        # Serialize the entire power change with responder handling, including
        # the state update before the kernel's ACK mask is reconfigured.
        session = self._session
        with session._lock if session is not None else nullcontext():
            previous = self._power
            if power == previous:
                return
            self._power = power
            if power != PowerState.ON:
                self.pressed_key = None
            try:
                if session is not None:
                    session.power_changed(self, previous)
            except BaseException:
                self._power = previous
                raise

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

    def announce(self):
        if self.power == PowerState.OFF:
            return
        self.send(15, 0xa6, [6, self.all_device_types, 0, 0])
        self.send(15, 0x84, self.address.physical.to_bytes(2, "big") + bytes([self.device_type]))
        self.send(15, 0x87, self.vendor_id.to_bytes(3, "big"))

    def receive(self, frame, *, transport="wire"):
        if transport not in ("wire", "virtual"):
            raise ValueError("CEC receive transport must be 'wire' or 'virtual'")
        if self.power == PowerState.OFF:
            return
        if frame.destination not in (self.address.logical, 15) or frame.source == self.address.logical:
            return
        if frame.timestamp_ns and frame.timestamp_ns < self._active_since_ns:
            return
        original = frame
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
        self.handle(frame)
        if self.hooks.after_rx is not None:
            self.hooks.after_rx(self, frame)

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
                self.send(15, 0xa6, [6, self.all_device_types, 0, 0])
                return
            if frame.source != 15:
                if opcode == 0x46 and not operands:
                    self.send(frame.source, 0x47, self.name.encode("ascii"))
                    return
                if opcode == 0x9f and not operands:
                    self.send(frame.source, 0x9e, [6])
                    return
                if opcode == 0x8f and not operands:
                    self.send(frame.source, 0x90, [0 if self.power == PowerState.ON else 1])
                    return
        if opcode == 0x36 and not operands:
            self.standby()
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

    def __init__(self, *args, direction="front", **kwargs):
        super().__init__(*args, direction=direction, **kwargs)
        self.active_input = None

    def handle(self, frame):
        if frame.opcode in (0x04, 0x0d) and frame.destination == self.address.logical and not frame.operands:
            self.power_on()
        elif frame.opcode in (0x82, 0x86) and frame.destination == 15 and len(frame.operands) == 2:
            self.active_input = int.from_bytes(frame.operands, "big")
        elif frame.opcode == 0x91 and frame.destination == self.address.logical and not frame.operands:
            self.send(15, 0x32, b"eng")
        else:
            super().handle(frame)


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


class PlaybackComponent(Component):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.active = False
        self.playing = False

    def set_power(self, power):
        super().set_power(power)
        if self.power != PowerState.ON:
            self.active = self.playing = False

    def activate(self):
        self.power_on()
        self.active = True
        self.send(15, 0x82, self.address.physical.to_bytes(2, "big"))

    def handle(self, frame):
        opcode, operands = frame.opcode, frame.operands
        if opcode == 0x85 and frame.destination == 15 and not operands:
            if self.active and self.power == PowerState.ON:
                self.send(15, 0x82, self.address.physical.to_bytes(2, "big"))
        elif opcode == 0x86 and frame.destination == 15 and len(operands) == 2:
            if int.from_bytes(operands, "big") == self.address.physical:
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
