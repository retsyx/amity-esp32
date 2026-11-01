"""Additional CEC attachments of one physical AVR, without joining wires."""

from .components import AvrComponent
from .connections import TentacleEndpoint
from .topology import CecAddress, child_address


def _physical_address(component):
    if component.address is not None:
        return component.address.physical
    if component.parent is not None:
        return child_address(_physical_address(component.parent), component.port)
    if component.device_type == 0:
        return 0
    if component.direction == "back":
        return child_address(0, component.tv_input)
    raise ValueError("An AVR without an assigned upstream address needs an explicit attachment address")


class _AvrField:
    """Keep device state shared while transport and wire evidence stay local."""

    def __init__(self, name):
        self.name = name

    def __get__(self, view, owner=None):
        if view is None:
            return self
        device = view.__dict__.get("_avr_device")
        return getattr(device, self.name) if device is not None else view.__dict__[self.name]

    def __set__(self, view, value):
        device = view.__dict__.get("_avr_device")
        if device is None:
            view.__dict__[self.name] = value
        else:
            setattr(device, self.name, value)


class AvrAttachment(AvrComponent):
    """The same AVR exposed to another physical DUT endpoint.

    An AVR output and an AVR input may connect to different Amity tentacles.
    Each bench GPIO remains an independent physical CEC transport, but both
    responders have one AVR identity, power state and selected input. Frames
    are never copied from one wire to another. Pass both the original AVR and
    this attachment to the same TestBed; scan locations remain distinct.
    """

    def __init__(self, avr, endpoint, *, address=None, hooks=None):
        if not isinstance(avr, AvrComponent) or isinstance(avr, AvrAttachment):
            raise TypeError("An AVR attachment needs its original AVR component")
        if not isinstance(endpoint, TentacleEndpoint):
            raise TypeError("An AVR attachment needs a DUT tentacle endpoint")
        if endpoint.gpio == avr.gpio:
            raise ValueError("The AVR already has an attachment on that CEC wire")
        expected = _physical_address(avr)
        address = address or CecAddress(5, expected)
        if address != CecAddress(5, expected):
            raise ValueError("An AVR attachment must retain the AVR's logical and physical addresses")
        super().__init__(endpoint, avr.name, address=address, power=avr.power,
                         vendor_id=avr.vendor_id, hooks=hooks)
        self._avr_device = avr
        views = getattr(avr, "_cec_views", None)
        if views is None:
            avr._cec_views = views = [avr]
        self._cec_views = views
        views.append(self)

    def announce(self):
        device = self._avr_device
        if (self._session is None or device._session is not self._session
                or device not in self._session.components):
            raise ValueError("All AVR attachments must belong to the same CEC session")
        if self.address != device.address:
            raise ValueError("An AVR attachment's address disagrees with the assigned AVR identity")
        super().announce()


for _name in (
        "_power", "_power_revision", "_power_calls", "_power_condition", "pressed_key",
        "_active_input", "_input_revision", "_input_calls", "_selected_input",
        "_switch_input_revision", "_switch_input_calls", "volume", "muted", "system_audio"):
    setattr(AvrAttachment, _name, _AvrField(_name))
