#!/usr/bin/env python3
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

"""Overlap repeated real CASE establishment with HTTPS status reads on the fixture."""
import argparse
import concurrent.futures
import json
from pathlib import Path
import shlex
import subprocess
import threading

from cec_web_activity import Admin, read_password, write_private


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', required=True)
    parser.add_argument('--credentials', type=Path, required=True, help='Private JSON with admin_password')
    parser.add_argument('--host', default='pi@10.0.1.119')
    parser.add_argument('--version', required=True, help='Expected firmware version')
    parser.add_argument('--output', type=Path, default=Path('test-results/web-matter-coexistence.json'))
    args = parser.parse_args()
    admin = Admin(args.url)
    report = {'passed': False, 'samples': [], 'matter': []}
    stop = threading.Event()
    root = '/home/pi/amity/esp32'
    ssh = ['ssh', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=yes',
           '-o', 'UserKnownHostsFile=test-results/harness_known_hosts', args.host]

    def sample():
        while not stop.is_set():
            state = admin.request('/api/status')
            assert state['version'] == args.version and state['ready']
            report['samples'].append({key: state[key] for key in
                ['free_heap', 'free_internal_heap', 'largest_internal_block', 'wifi_rssi']})

    try:
        admin.login(read_password(args.credentials))
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            reads = pool.submit(sample)
            try:
                for iteration in range(3):
                    command = shlex.join(['sudo', '-n', 'unshare', '--mount', '--propagation', 'private',
                        'env', 'PYTHONDONTWRITEBYTECODE=1', f'PYTHONPATH={root}/.matter-python', 'python3',
                        f'{root}/tests/matter_pi_controller.py', '--storage', f'{root}/test-results/matter',
                        '--trust-store', f'{root}/paa-root-certs', '--isolate-platform-tmp',
                        '--expected-version', args.version])
                    subprocess.run(ssh + [f'umask 077; {command} > {root}/test-results/matter/coexistence-{iteration}.log 2>&1'],
                                   check=True, capture_output=True, text=True, timeout=80)
                    result = json.loads(subprocess.check_output(ssh + [f'sudo -n cat {root}/test-results/matter/result.json'], text=True))
                    assert result['passed']
                    report['matter'].append(result)
            finally:
                stop.set()
            reads.result()
        assert len(report['samples']) >= 3
        report['passed'] = True
        print('PASS concurrent HTTPS reads and three CASE sessions;', len(report['samples']), 'complete responses')
        print('Minimum internal free heap:', min(s['free_internal_heap'] for s in report['samples']))
    finally:
        stop.set()
        if admin.token:
            admin.request('/api/logout', {})
        write_private(args.output, report)


if __name__ == '__main__':
    main()
