#!/usr/bin/env python3
"""Keep board menus and release tooling within shared limits."""
from pathlib import Path
import re
import runpy
import unittest

ROOT = Path(__file__).resolve().parents[1]


def capacity(header):
    values = re.findall(r'^#define\s+AMITY_MAX_TENTACLES\s+([1-9][0-9]*)\s*$', header, re.M)
    assert len(values) == 1, 'Missing or ambiguous canonical hardware capacity'
    return int(values[0])


def check_kconfig(maximum, source):
    profiles = re.findall(r'AMITY_BOARD_DEVELOPMENT_([0-9]+)T\b', source)
    assert profiles and all(int(count) == maximum for count in profiles), 'Development profile capacity differs'
    menus = [(int(label), int(slot)) for label, slot in re.findall(
        r'^menu "HDMI adapter ([0-9]+) \(stable connector ([0-9]+)\)"$', source, re.M)]
    assert menus == [(slot + 1, slot) for slot in range(maximum)], 'Connector menus differ'
    expected = {f'AMITY_TENTACLE{slot}_{side}_GPIO' for slot in range(maximum) for side in ('FRONT', 'BACK')}
    fields = re.findall(r'^config (AMITY_TENTACLE[0-9]+_[A-Z_]+)$', source, re.M)
    assert len(fields) == len(expected) and set(fields) == expected, 'Connector GPIO fields differ'
    assert all(int(slot) < maximum for slot in re.findall(r'AMITY_TENTACLE([0-9]+)_', source)), 'Out-of-range connector reference'
    assert re.search(r'^\s*default AMITY_BOARD_LEGACY_1T\s*$', source, re.M), 'Default board must retain one adapter'


def check_tooling(maximum, tooling):
    assert tooling['MAX_TENTACLES'] == maximum and tooling['MAX_CEC_PORTS'] == 2 * maximum, 'Release tooling capacity differs'
    assert tooling['LEGACY'] == 'esp32s3-legacy-1t-v1', 'Legacy profile identity differs'
    assert tooling['DEVELOPMENT'] == f'esp32s3-development-{maximum}t-v1', 'Development profile identity differs'
    assert tooling['BOARD_FLAGS'] == ('AMITY_BOARD_LEGACY_1T', f'AMITY_BOARD_DEVELOPMENT_{maximum}T'), 'Board flags differ'


class HardwareCapacityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.header = (ROOT / 'components/amity_system/include/amity/hardware_limits.hpp').read_text()
        cls.maximum = capacity(cls.header)
        cls.kconfig = (ROOT / 'main/Kconfig.projbuild').read_text()
        cls.tooling = runpy.run_path(str(ROOT / 'tools/hardware_profile.py'))

    def test_production_sources_agree(self):
        check_kconfig(self.maximum, self.kconfig)
        check_tooling(self.maximum, self.tooling)


if __name__ == '__main__':
    unittest.main()
