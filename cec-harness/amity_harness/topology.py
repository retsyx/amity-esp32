"""CEC logical addresses and HDMI physical addresses for test topologies."""

from dataclasses import dataclass


def physical_address(value):
    """Convert a dotted HDMI physical address (or integer) to its wire value."""
    if isinstance(value, str):
        parts = value.split(".")
        if len(parts) != 4 or any(len(part) != 1 for part in parts):
            raise ValueError("A physical address must have four hexadecimal nibbles")
        try:
            value = int("".join(parts), 16)
        except ValueError as exc:
            raise ValueError("Invalid HDMI physical address") from exc
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value < 0xffff:
        raise ValueError("Invalid HDMI physical address")
    zero_seen = False
    for shift in (12, 8, 4, 0):
        nibble = (value >> shift) & 15
        if zero_seen and nibble:
            raise ValueError("Physical address has a nonzero port after a zero port")
        zero_seen |= nibble == 0
    return value


def format_physical_address(value):
    value = physical_address(value)
    return ".".join(f"{(value >> shift) & 15:x}" for shift in (12, 8, 4, 0))


@dataclass(frozen=True)
class CecAddress:
    logical: int
    physical: int | str

    def __post_init__(self):
        if isinstance(self.logical, bool) or not isinstance(self.logical, int) or not 0 <= self.logical <= 14:
            raise ValueError("A component needs a registered logical address from 0 through 14")
        object.__setattr__(self, "physical", physical_address(self.physical))


def child_address(parent, port):
    parent = physical_address(parent)
    if isinstance(port, bool) or not isinstance(port, int) or not 1 <= port <= 15:
        raise ValueError("HDMI ports are numbered 1 through 15")
    for shift in (12, 8, 4, 0):
        if not (parent >> shift) & 15:
            return physical_address(parent | (port << shift))
    raise ValueError("HDMI topology exceeds four physical-address levels")


def assign_addresses(components, *, reserved=None):
    """Assign by device type and parent/port; detect collisions before any I/O.

    Logical addresses are scoped to isolated GPIO wires. Physical addresses are
    scoped to the connected topology (a TV root and all its descendants), so a
    parent can sit across the DUT's front/back isolation boundary. ``reserved``
    maps GPIO numbers to already occupied logical addresses, for example a DUT.
    Explicit CecAddress values are preserved and validated against the tree.
    """
    components = list(components)
    if len(set(components)) != len(components):
        raise ValueError("A component occurs more than once in the topology")
    members = set(components)
    used = {gpio: set(addresses) for gpio, addresses in (reserved or {}).items()}
    roots, visiting, physicals = {}, set(), {}

    def locate(component):
        if component in roots:
            return roots[component]
        if component in visiting:
            raise ValueError("HDMI topology contains a parent cycle")
        visiting.add(component)
        parent = component.parent
        if parent is None:
            if component.port is not None:
                raise ValueError("A root component cannot specify an HDMI input port")
            root = component
            if component.address is None and component.device_type != 0:
                raise ValueError("A non-TV root needs an explicit physical address")
            pa = component.address.physical if component.address else 0
            if component.device_type == 0 and pa != 0:
                raise ValueError("A TV root must have physical address 0.0.0.0")
        else:
            if parent not in members:
                raise ValueError("Every parent must be included in the topology")
            root = locate(parent)
            if component.port is None:
                raise ValueError("A child component needs its parent's HDMI input port")
            if parent.device_type not in (0, 5) and not parent.can_route:
                raise ValueError("The parent component cannot route HDMI inputs")
            pa = child_address(physicals[parent], component.port)
            if component.address and component.address.physical != pa:
                raise ValueError("Explicit physical address disagrees with parent/port topology")
        roots[component], physicals[component] = root, pa
        visiting.remove(component)
        return root

    for component in components:
        locate(component)
    occupied_pa = set()
    for component in components:
        key = roots[component], physicals[component]
        if key in occupied_pa:
            raise ValueError("Two components occupy the same HDMI physical address")
        occupied_pa.add(key)
        if component.address:
            logical = component.address.logical
            if logical not in component.logical_addresses:
                raise ValueError("Logical address does not match component type")
            wire = used.setdefault(component.gpio, set())
            if logical in wire:
                raise ValueError("Logical address collision on an isolated CEC wire")
            wire.add(logical)
    assigned = {}
    for component in components:
        if component.address is None:
            wire = used.setdefault(component.gpio, set())
            logical = next((la for la in component.logical_addresses if la not in wire), None)
            if logical is None:
                raise ValueError(f"No logical address available for {component.name}")
            wire.add(logical)
            component.address = CecAddress(logical, physicals[component])
        assigned[component] = component.address
    return assigned
