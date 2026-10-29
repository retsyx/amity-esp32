"""Physical DUT HDMI attachment points, separate from component input ports."""

from dataclasses import dataclass, field


def _index(value):
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValueError("Tentacle must be a nonnegative integer")
    return value


def hdmi_port(value):
    if isinstance(value, bool) or not isinstance(value, int) or not 1 <= value <= 15:
        raise ValueError("HDMI input ports must be integers from 1 through 15")
    return value


@dataclass(frozen=True, eq=False)
class TentacleEndpoint:
    """One isolated CEC wire at a DUT's physical HDMI tentacle.

    ``tv_input`` identifies the upstream TV input carrying this inline HDMI
    connection; the DUT itself adds no HDMI physical-address level. GPIO is
    resolved exclusively from the DUT's already parsed Markdown wiring table.
    Constructing an endpoint performs no HTTP, GPIO or other hardware I/O.
    """

    dut: object
    tentacle: int
    direction: str
    tv_input: int = 1
    gpio: int = field(init=False)

    def __post_init__(self):
        _index(self.tentacle)
        hdmi_port(self.tv_input)
        if self.direction not in ("front", "back"):
            raise ValueError("Direction must be 'front' or 'back'")
        try:
            name, wiring = self.dut.name, self.dut.wiring
            gpio = wiring.cec_gpio(name, self.tentacle, self.direction)
        except AttributeError as exc:
            raise TypeError("A tentacle endpoint needs a DUT with parsed wiring") from exc
        object.__setattr__(self, "gpio", gpio)

    def __eq__(self, other):
        if not isinstance(other, TentacleEndpoint):
            return NotImplemented
        return (self.dut is other.dut and self.tentacle == other.tentacle
                and self.direction == other.direction and self.tv_input == other.tv_input)

    def __hash__(self):
        return hash((id(self.dut), self.tentacle, self.direction, self.tv_input))


@dataclass(frozen=True)
class Tentacle:
    """Paired front/back HDMI endpoints, retaining their CEC isolation."""

    dut: object
    index: int = 0
    tv_input: int = 1
    front: TentacleEndpoint = field(init=False)
    back: TentacleEndpoint = field(init=False)

    def __post_init__(self):
        _index(self.index)
        hdmi_port(self.tv_input)
        object.__setattr__(self, "front", TentacleEndpoint(self.dut, self.index, "front", self.tv_input))
        object.__setattr__(self, "back", TentacleEndpoint(self.dut, self.index, "back", self.tv_input))
