"""Generate Pi 5 boot settings from the harness's documented wiring."""

import re


_BEGIN = "# BEGIN Amity harness"
_END = "# END Amity harness"
_OVERLAY = "amity-harness"


def _pins(wiring):
    cec, reset = list(wiring.cec.values()), list(wiring.reset.values())
    if not cec or not reset:
        raise ValueError("Harness wiring needs CEC and setup/reset pins")
    if any(type(pin) is not int or not 0 <= pin <= 27 for pin in cec + reset):
        raise ValueError("Harness GPIOs must be Pi header GPIO numbers from 0 to 27")
    if len(set(cec + reset)) != len(cec + reset):
        raise ValueError("Harness GPIO assignments overlap")
    return sorted(cec), sorted(reset)


def overlay_source(wiring):
    """Describe one open-drain CEC adapter per wire, with no internal pulls.

    Linux uses the decimal node names as adapter identities (``cec@23``),
    matching the framework's lookup regardless of ``/dev/cecN`` numbering.
    """
    cec, _ = _pins(wiring)
    pin_groups = "\n".join(
        f'''            amity_cec_{pin}: amity-cec-{pin} {{
                pins = "gpio{pin}";
                function = "gpio";
                bias-disable;
            }};''' for pin in cec)
    devices = "\n".join(
        f'''            cec@{pin} {{
                compatible = "cec-gpio";
                // GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN.
                cec-gpios = <&rp1_gpio {pin} 6>;
                pinctrl-names = "default";
                pinctrl-0 = <&amity_cec_{pin}>;
                status = "okay";
            }};''' for pin in cec)
    return f'''// Generated from TEST-HARNESS.md; edit the wiring table, not this file.
/dts-v1/;
/plugin/;

/ {{
    compatible = "raspberrypi,5-model-b";
    fragment@0 {{
        target = <&rp1_gpio>;
        __overlay__ {{
{pin_groups}
        }};
    }};
    fragment@1 {{
        target-path = "/";
        __overlay__ {{
{devices}
        }};
    }};
}};
'''


def boot_config(existing_text, wiring=None):
    """Replace our managed block and old CEC overlay directives exactly once.

    An explicit ``[all]`` makes setup independent of the last existing model
    filter. Other boot settings and commented directives remain intact.
    """
    if wiring is not None:
        cec, reset = _pins(wiring)
    lines, inside = [], False
    for line in existing_text.splitlines(keepends=True):
        stripped = line.strip()
        if stripped == _BEGIN:
            if inside:
                raise ValueError("Nested Amity harness boot configuration block")
            inside = True
        elif stripped == _END:
            if not inside:
                raise ValueError("Unmatched Amity harness boot configuration end")
            inside = False
        elif not inside:
            # Previous installations may have one cec-gpio overlay per wire.
            # Leaving those active would create duplicate adapters on the pins.
            if not re.match(r"^\s*dtoverlay\s*=\s*(?:cec-gpio|amity-harness)(?:\s*,|\s*(?:#.*)?$)", line):
                lines.append(line)
    if inside:
        raise ValueError("Unterminated Amity harness boot configuration block")
    settings = [_BEGIN, "[all]"]
    if wiring is not None:
        settings.append("gpio=" + ",".join(str(pin) for pin in sorted(cec + reset)) + "=ip,np")
    settings += ["dtoverlay=" + _OVERLAY, _END]
    prefix = "".join(lines).rstrip("\r\n")
    return (prefix + "\n\n" if prefix else "") + "\n".join(settings) + "\n"
