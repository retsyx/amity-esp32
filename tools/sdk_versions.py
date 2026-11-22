#!/usr/bin/env python3
"""Print the pinned SDK commits from SDK_VERSIONS.txt as name=commit lines.

The lines are idf, matter and chip, in the form GitHub Actions step outputs use.
"""
from pathlib import Path
import re


def main():
    versions = (Path(__file__).resolve().parents[1] / 'SDK_VERSIONS.txt').read_text()
    for name, label in (('idf', 'ESP-IDF'), ('matter', 'ESP-Matter'), ('chip', 'ConnectedHomeIP')):
        match = re.search(rf'^{label}(?: [^:]+)?: ([0-9a-f]{{40}})$', versions, re.MULTILINE)
        if match is None:
            raise SystemExit(f'Missing pinned {label} commit in SDK_VERSIONS.txt')
        print(f'{name}={match[1]}')


if __name__ == '__main__':
    main()
