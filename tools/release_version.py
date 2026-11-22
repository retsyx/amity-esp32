#!/usr/bin/env python3
"""Check that a release tag is v followed by PROJECT_VER from CMakeLists.txt.

Usage: release_version.py <tag>
"""
from pathlib import Path
import re
import sys


def main():
    tag = sys.argv[1]
    project = (Path(__file__).resolve().parents[1] / 'CMakeLists.txt').read_text()
    version = re.search(r'^set\(PROJECT_VER "([^"]+)"', project, re.MULTILINE)
    if version is None or tag != 'v' + version[1]:
        raise SystemExit('Release tag must be v followed by PROJECT_VER from CMakeLists.txt')
    print(f'Releasing Amity {version[1]} from {tag}')


if __name__ == '__main__':
    main()
