#!/usr/bin/env python3
"""Closed, nonsecret build/GPIO identity shared by offline release tools."""
import hashlib
import json
from pathlib import Path
import re

_limits_header = Path(__file__).resolve().parents[1] / 'components/amity_system/include/amity/hardware_limits.hpp'
MAX_TENTACLES = int(re.search(r'^#define AMITY_MAX_TENTACLES ([1-9][0-9]*)$', _limits_header.read_text(), re.MULTILINE)[1])
MAX_CEC_PORTS = 2 * MAX_TENTACLES
LEGACY = 'esp32s3-legacy-1t-v1'
DEVELOPMENT = f'esp32s3-development-{MAX_TENTACLES}t-v1'
START = b'AMITY_HARDWARE_BEGIN_V1\n'
END = b'AMITY_HARDWARE_END_V1\n'
BOARD_FLAGS = ('AMITY_BOARD_LEGACY_1T', f'AMITY_BOARD_DEVELOPMENT_{MAX_TENTACLES}T')


def read_config(path):
    path = Path(path)
    if path.suffix == '.json':
        source = json.loads(path.read_text())
        return {key.removeprefix('CONFIG_'): ('y' if value is True else 'n' if value is False else str(value))
                for key, value in source.items()}
    result = {}
    for line in path.read_text().splitlines():
        if line.startswith('CONFIG_') and '=' in line:
            key, value = line.split('=', 1)
            if key[7:] in result:
                raise ValueError('Duplicate SDK configuration key')
            result[key[7:]] = value
        elif line.startswith('# CONFIG_') and line.endswith(' is not set'):
            result[line[9:-11]] = 'n'
    return result


def integer(value):
    if not isinstance(value, str) or not re.fullmatch(r'-1|0|[1-9][0-9]*', value):
        raise ValueError('GPIO identity requires canonical integer values')
    number = int(value)
    if not -1 <= number <= 48:
        raise ValueError('GPIO identity is outside the ESP32-S3 range')
    return number


def boolean(value):
    if value not in ('y', 'n', '1', '0', None):
        raise ValueError('GPIO identity requires boolean presence settings')
    return int(value in ('y', '1'))


def descriptor(config):
    legacy, development = (boolean(config.get(key)) for key in BOARD_FLAGS)
    if legacy + development != 1:
        raise ValueError('Select exactly one explicit SDK board profile')
    if legacy:
        return START + (f'profile={LEGACY}\nrevision=1\ncapacity=1\nreset=4\nstatus=-1\n'
                        'slot0=1,2,-1,1,0,0\n').encode() + END
    reset = integer(config.get('AMITY_BOARD_RESET_GPIO'))
    lines = [f'profile={DEVELOPMENT}', 'revision=1', f'capacity={MAX_TENTACLES}', f'reset={reset}', 'status=-1']
    assigned = [] if reset < 0 else [reset]
    forbidden = {0, 3, 19, 20, 45, 46, *range(22, 38)}
    for slot in range(MAX_TENTACLES):
        prefix = f'AMITY_TENTACLE{slot}_'
        # Kconfig omits the dependent pull-down option when pull-up is enabled.
        if (any(prefix + key not in config for key in ('PRESENT_HIGH', 'PRESENCE_PULLUP')) or
                (prefix + 'PRESENCE_PULLDOWN' not in config and not boolean(config.get(prefix + 'PRESENCE_PULLUP')))):
            raise ValueError('SDK configuration omits presence-input settings')
        front, back, presence = (integer(config.get(prefix + key)) for key in ('FRONT_GPIO', 'BACK_GPIO', 'PRESENCE_GPIO'))
        high, pullup, pulldown = (boolean(config.get(prefix + key)) for key in ('PRESENT_HIGH', 'PRESENCE_PULLUP', 'PRESENCE_PULLDOWN'))
        if (front < 0) != (back < 0) or (presence >= 0 and front < 0) or pullup + pulldown > 1:
            raise ValueError('Invalid paired GPIO or presence-input profile')
        assigned.extend(pin for pin in (front, back, presence) if pin >= 0)
        lines.append(f'slot{slot}={front},{back},{presence},{high},{pullup},{pulldown}')
    if len(assigned) != len(set(assigned)) or forbidden.intersection(assigned):
        raise ValueError('GPIO profile has a pin collision or reserved peripheral pin')
    return START + ('\n'.join(lines) + '\n').encode() + END


def identity(raw):
    """Reject unknown, malformed, reordered or ambiguous embedded identities."""
    if not raw.startswith(START) or not raw.endswith(END) or len(raw) > 1024:
        raise ValueError('Malformed hardware descriptor')
    try:
        lines = raw[len(START):-len(END)].decode('ascii').splitlines()
        values = dict(line.split('=', 1) for line in lines)
    except (UnicodeDecodeError, ValueError) as error:
        raise ValueError('Malformed hardware descriptor') from error
    if len(values) != len(lines) or values.get('revision') != '1' or values.get('status') != '-1':
        raise ValueError('Duplicate or unsupported hardware descriptor fields')
    profile = values.get('profile')
    if profile == LEGACY:
        config = {BOARD_FLAGS[0]: 'y', BOARD_FLAGS[1]: 'n'}
    elif profile == DEVELOPMENT and values.get('capacity') == str(MAX_TENTACLES):
        config = {BOARD_FLAGS[0]: 'n', BOARD_FLAGS[1]: 'y', 'AMITY_BOARD_RESET_GPIO': values.get('reset')}
        for slot in range(MAX_TENTACLES):
            items = values.get(f'slot{slot}', '').split(',')
            if len(items) != 6 or any(item not in ('0', '1') for item in items[3:]):
                raise ValueError('Malformed hardware slot descriptor')
            for name, value in zip(('FRONT_GPIO', 'BACK_GPIO', 'PRESENCE_GPIO', 'PRESENT_HIGH', 'PRESENCE_PULLUP', 'PRESENCE_PULLDOWN'), items):
                config[f'AMITY_TENTACLE{slot}_' + name] = value
    else:
        raise ValueError('Unknown hardware profile')
    if descriptor(config) != raw:
        raise ValueError('Hardware descriptor is not the exact closed canonical profile')
    return profile, hashlib.sha256(raw).hexdigest() if profile == DEVELOPMENT else ''


def image_identity(data, config=None):
    if data.count(START) != 1 or data.count(END) != 1:
        raise ValueError('Image must contain exactly one compiled hardware descriptor')
    first = data.index(START)
    last = data.index(END, first) + len(END)
    raw = data[first:last]
    result = identity(raw)
    if config is not None and descriptor(config) != raw:
        raise ValueError('Image hardware identity differs from the selected SDK build configuration')
    return result
