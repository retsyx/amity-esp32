"""Read the bench wiring from its single, human-maintained Markdown source."""

from dataclasses import dataclass
from pathlib import Path
import re
from types import MappingProxyType


DEFAULT_WIRING = Path(__file__).resolve().parents[1] / "TEST-HARNESS.md"


def _text(value):
    return re.sub(r"[`*_]", "", value).strip()


def _section(document, title):
    headings = list(re.finditer(r"^\s{0,3}(#{1,6})\s+(.+?)\s*#*\s*$", document, re.M))
    matches = [index for index, match in enumerate(headings) if _text(match[2]) == title]
    if len(matches) != 1:
        raise ValueError(f"Expected one '{title}' heading, found {len(matches)}")
    index = matches[0]
    start = headings[index]
    end = next((match.start() for match in headings[index + 1:]
                if len(match[1]) <= len(start[1])), len(document))
    return document[start.end():end]


def _table(document, title, required):
    lines = _section(document, title).splitlines()
    tables = []
    for index, line in enumerate(lines[:-1]):
        if not line.lstrip().startswith("|"):
            continue
        header = [_text(cell) for cell in line.strip().strip("|").split("|")]
        separator = lines[index + 1].strip().strip("|").split("|")
        if len(separator) != len(header) or not all(re.fullmatch(r"\s*:?-+:?\s*", cell) for cell in separator):
            continue
        if set(required).issubset(header):
            if len(set(header)) != len(header):
                raise ValueError(f"Duplicate columns in '{title}'")
            rows = []
            for row in lines[index + 2:]:
                if not row.lstrip().startswith("|"):
                    break
                cells = [_text(cell) for cell in row.strip().strip("|").split("|")]
                if len(cells) != len(header):
                    raise ValueError(f"Wrong number of cells in '{title}': {row}")
                rows.append(dict(zip(header, cells)))
            tables.append(rows)
    if len(tables) != 1 or not tables[0]:
        raise ValueError(f"Expected one nonempty wiring table in '{title}'")
    return tables[0]


def _integer(value, description):
    if not re.fullmatch(r"[0-9]+", value):
        raise ValueError(f"Invalid {description}: {value!r}")
    return int(value)


@dataclass(frozen=True)
class DeviceIdentity:
    name: str
    mac: str
    device_id: str


class Wiring:
    """Validated ``(DUT name, tentacle, front/back) -> Pi GPIO`` lookup.

    No Linux ``/dev/cecN`` numbering or virtual equipment configuration is
    stored here. Those are runtime details, independent of the bench wiring.
    """

    def __init__(self, identities, cec, reset, source=None):
        self.identities = MappingProxyType(dict(identities))
        self.cec = MappingProxyType(dict(cec))
        self.reset = MappingProxyType(dict(reset))
        self.source = source

    @classmethod
    def from_markdown(cls, path=DEFAULT_WIRING):
        path = Path(path)
        document = path.read_text(encoding="utf-8")
        identities = {}
        for row in _table(document, "DUTs", ("Bench label", "ESP32-S3 MAC / USB serial", "Amity device ID")):
            name = row["Bench label"]
            mac = row["ESP32-S3 MAC / USB serial"]
            device_id = row["Amity device ID"]
            if not name or name in identities:
                raise ValueError(f"Duplicate or empty DUT name: {name!r}")
            if not re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", mac):
                raise ValueError(f"Invalid MAC address for {name}")
            if not re.fullmatch(r"[0-9a-fA-F]{32}", device_id):
                raise ValueError(f"Invalid Amity device ID for {name}")
            if any(item.mac.lower() == mac.lower() or item.device_id.lower() == device_id.lower()
                   for item in identities.values()):
                raise ValueError(f"Duplicate DUT identity for {name}")
            identities[name] = DeviceIdentity(name, mac, device_id)
        cec, reset, used_gpio = {}, {}, {}
        for row in _table(document, "HDMI-CEC harness GPIO mapping",
                          ("Device", "Device Tentacle #", "Tentacle Front/Back", "DUT GPIO", "Harness GPIO")):
            name = row["Device"]
            if name not in identities:
                raise ValueError(f"CEC wiring references unknown DUT {name!r}")
            side = row["Tentacle Front/Back"].lower()
            if side not in ("front", "back"):
                raise ValueError(f"Invalid CEC direction {side!r} for {name}")
            tentacle = _integer(row["Device Tentacle #"], "tentacle")
            _integer(row["DUT GPIO"], "DUT GPIO")
            gpio = _integer(row["Harness GPIO"], "harness GPIO")
            key = name, tentacle, side
            if key in cec:
                raise ValueError(f"Duplicate CEC wiring for {key}")
            if gpio in used_gpio:
                raise ValueError(f"Harness GPIO {gpio} is assigned more than once")
            cec[key] = gpio
            used_gpio[gpio] = key
        for row in _table(document, "Setup/reset GPIO wiring", ("Bench label", "Harness GPIO (BCM/RP1)")):
            name = row["Bench label"]
            gpio = _integer(row["Harness GPIO (BCM/RP1)"], "setup/reset GPIO")
            if name not in identities or name in reset:
                raise ValueError(f"Unknown or duplicate setup/reset DUT {name!r}")
            if gpio in used_gpio:
                raise ValueError(f"Harness GPIO {gpio} is assigned more than once")
            reset[name] = gpio
            used_gpio[gpio] = name
        if set(reset) != set(identities) or {key[0] for key in cec} != set(identities):
            raise ValueError("Every DUT needs CEC and setup/reset wiring")
        for name, tentacle, side in cec:
            if (name, tentacle, "back" if side == "front" else "front") not in cec:
                raise ValueError(f"Missing paired CEC side for {name} tentacle {tentacle}")
        return cls(identities, cec, reset, path)

    def cec_gpio(self, name, tentacle=0, direction="front"):
        if isinstance(tentacle, bool) or not isinstance(tentacle, int) or tentacle < 0:
            raise ValueError("Tentacle must be a nonnegative integer")
        if direction not in ("front", "back"):
            raise ValueError("Direction must be 'front' or 'back'")
        try:
            return self.cec[name, tentacle, direction]
        except KeyError:
            raise ValueError(f"No CEC wiring for {name!r}, tentacle {tentacle}, {direction}") from None

    def reset_gpio(self, name):
        try:
            return self.reset[name]
        except KeyError:
            raise ValueError(f"No setup/reset wiring for {name!r}") from None
