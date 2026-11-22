#!/usr/bin/env python3
"""Run the CI release-tag and SDK-pin scripts against the repository files."""
from pathlib import Path
import re
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]


def run(name, *args):
    return subprocess.run([sys.executable, str(ROOT / 'tools' / name), *args], capture_output=True, text=True)


class ReleaseScriptTests(unittest.TestCase):
    def test_sdk_versions_prints_each_pinned_commit(self):
        result = run('sdk_versions.py')
        self.assertEqual(result.returncode, 0, result.stderr)
        pins = dict(line.split('=', 1) for line in result.stdout.splitlines())
        versions = (ROOT / 'SDK_VERSIONS.txt').read_text()
        for name, label in (('idf', 'ESP-IDF'), ('matter', 'ESP-Matter'), ('chip', 'ConnectedHomeIP')):
            self.assertRegex(versions, rf'(?m)^{label}[^:\n]*: {pins.pop(name)}$')
        self.assertEqual(pins, {})

    def test_release_tag_must_be_v_and_project_version(self):
        version = re.search(r'(?m)^set\(PROJECT_VER "([^"]+)"', (ROOT / 'CMakeLists.txt').read_text())[1]
        self.assertEqual(run('release_version.py', 'v' + version).returncode, 0)
        for tag in (version, 'v' + version + '-rc1', 'v' + version + '.1'):
            self.assertNotEqual(run('release_version.py', tag).returncode, 0, tag)


if __name__ == '__main__':
    unittest.main()
