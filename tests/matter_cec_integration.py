#!/usr/bin/env python3
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

"""Verify operational Matter controls against the real two-bus CEC fixture.

Requires eight fixture activities and an already commissioned Pi controller.
All controller output stays in the private fixture directory. No provisioning.
"""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import time

from cec_hardware import Harness, raw_frames
from cec_web_activity import Admin, read_password, write_private


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--url', required=True)
    p.add_argument('--credentials', type=Path, required=True, help='Private JSON with admin_password')
    p.add_argument('--host', default='pi@10.0.1.119')
    p.add_argument('--version', required=True, help='Expected firmware version')
    p.add_argument('--output', type=Path, default=Path('test-results/matter-cec.json'))
    args = p.parse_args()
    admin = Admin(args.url)
    harness = Harness(args.host)
    remote = '/home/pi/amity/esp32'
    ssh = ['ssh', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=yes',
           '-o', 'UserKnownHostsFile=test-results/harness_known_hosts', args.host]
    report = {'passed': False, 'checks': [], 'controllers': {}, 'captures': {}}

    def check(name, condition):
        assert condition, name
        report['checks'].append(name)
        print('PASS', name, flush=True)

    def controller(name, *options):
        command = shlex.join(['sudo', '-n', 'unshare', '--mount', '--propagation', 'private',
            'env', 'PYTHONDONTWRITEBYTECODE=1', f'PYTHONPATH={remote}/.matter-python',
            'python3', f'{remote}/tests/matter_pi_controller.py', '--storage', f'{remote}/test-results/matter',
            '--trust-store', f'{remote}/paa-root-certs', '--isolate-platform-tmp',
            '--expected-version', args.version, '--expect-input-count', '8', *options])
        log = shlex.quote(f'{remote}/test-results/matter/cec-{name}.log')
        run = subprocess.run(ssh + [f'umask 077; {command} > {log} 2>&1'], capture_output=True, text=True, timeout=100)
        result = json.loads(subprocess.check_output(ssh + [f'sudo -n cat {remote}/test-results/matter/result.json'], text=True))
        report['controllers'][name] = result
        check('Matter ' + name, run.returncode == 0 and result.get('passed'))
        return result

    def capture(name, duration, operation):
        command = shlex.join(['sudo', '-n', 'cec-ctl', '-d', '/dev/cec1', '-s', '-r', '-v', '-M',
                              '--monitor-time', str(duration)])
        process = subprocess.Popen(ssh + [command], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            time.sleep(.7)
            operation()
            stdout, stderr = process.communicate(timeout=duration + 5)
            check(name + ' capture', process.returncode == 0 and 'failed' not in stderr.lower())
            report['captures'][name] = stdout
            return raw_frames(stdout)
        finally:
            if process.poll() is None:
                process.terminate()
                process.communicate(timeout=5)

    try:
        admin.login(read_password(args.credentials))
        admin.activity(0)
        state = admin.request('/api/status')
        destination = state['back_address'] << 4 | 4
        frames = capture('controls', 25, lambda: controller('controls', '--exercise-controls', '--exercise-power-inputs'))
        expected = [0x44, 0x46, 0x45, 0x4c, 0x4b, 0, 1, 2, 3, 4, 9, 11, 13, *range(0x20, 0x2a), 0x41, 0x42]
        for key in expected:
            check(f'CEC control {key:02x}', bytes([destination, 0x44, key]) in frames)
        check('CEC key releases', frames.count(bytes([destination, 0x45])) >= len(expected))
        for muted, key in ((True, 0x65), (False, 0x66)):
            # Inject a real CEC Report Audio Status from the selected audio peer.
            harness.cec('/dev/cec1', '-t', state['back_address'], '--custom-command',
                        f'cmd=0x7a,payload=0x{(0x80 if muted else 0) | 50:02x}')
            frames = capture('mute-' + str(muted), 8, lambda: controller('mute-' + str(muted),
                '--expect-volume', '127', '--expect-muted', 'yes' if muted else 'no',
                '--speaker-mute', 'muted' if muted else 'unmuted'))
            check('explicit mute command ' + str(muted), bytes([destination, 0x44, key]) in frames)
        for index, name in ((1, 'activity-change'), (-1, 'standby')):
            admin.activity(0)
            # Controller return follows the accepted Step command. Its shutdown
            # is immediate; 20 steps take five seconds, leaving time to interrupt.
            def interrupt():
                controller(name, '--volume-step', '20')
                time.sleep(.3)
                admin.activity(index)
            early = capture(name, 10, interrupt)
            count = early.count(bytes([destination, 0x44, 0x41]))
            check(name + ' cancels pending steps', 0 < count < 20)
            late = capture(name + '-settled', 3, lambda: None)
            check(name + ' has no stale volume commands', bytes([destination, 0x44, 0x41]) not in late)
        report['passed'] = True
    finally:
        if admin.token:
            admin.activity(-1)
            admin.request('/api/logout', {})
        write_private(args.output, report)


if __name__ == '__main__':
    main()
