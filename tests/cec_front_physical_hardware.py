#!/usr/bin/env python3
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

"""Check per-boot front physical-address discovery on the isolated Pi/DUT fixture.

Configures the Pi's two cec-gpio adapters and reboots the DUT. Optionally installs
an image through authenticated OTA first. No activities or defaults are changed.
Leaves a synthetic TV/source configured at --restore-physical and reboots the
DUT to rediscover that source. Credentials and tokens are never printed.
"""
import argparse
import json
import time
import urllib.request
from pathlib import Path

from cec_hardware import Harness
from cec_web_activity import Admin, read_password, write_private


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', required=True)
    parser.add_argument('--credentials', type=Path, required=True)
    parser.add_argument('--host', required=True)
    parser.add_argument('--ssh-port', type=int)
    parser.add_argument('--front-adapter', default='/dev/cec0')
    parser.add_argument('--back-adapter', default='/dev/cec1')
    parser.add_argument('--restore-physical', default='1.0.0.0')
    parser.add_argument('--firmware', type=Path)
    parser.add_argument('--output', type=Path, default=Path('test-results/cec-front-physical-hardware.json'))
    args = parser.parse_args()
    admin = Admin(args.url)
    password = read_password(args.credentials)
    harness = Harness(args.host, args.ssh_port)
    report = {'passed': False, 'checks': [], 'http': admin.records, 'cec': harness.records}
    configured = False

    def check(name, condition):
        if not condition:
            raise AssertionError(name)
        report['checks'].append(name)
        print('PASS', name, flush=True)

    def source(physical):
        harness.cec(args.back_adapter, '--clear', '--playback', '--phys-addr', physical,
                    '--osd-name', 'HarnessSource', '--vendor-id', '0x000002')

    def boot(action=None):
        admin.login(password)
        previous = admin.request('/api/setup')['boot_id']
        if action:
            action()
        else:
            admin.request('/api/reboot', {})
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            time.sleep(.5)
            try:
                setup = admin.request('/api/setup')
                if setup['boot_id'] == previous or not setup['ready']:
                    continue
                admin.login(password)
                return admin.wait_status(lambda state: state['ready'] and not state['discovering'], timeout=45)
            except (OSError, ValueError):
                continue
        raise AssertionError('DUT did not complete a fresh healthy boot')

    def expect_physical(state, physical):
        check('status reports front address ' + physical, state.get('determined_front_physical_address') == physical)
        value = int(physical.replace('.', ''), 16)
        harness.expect(args.front_adapter, state['front_address'], '--give-physical-addr',
                       bytes([0x84, value >> 8, value & 255, 4]))
        check('wire identity reports front address ' + physical, True)

    def rescan():
        generation = admin.request('/api/status')['discovery_generation']
        admin.request('/api/discover', {})
        return admin.wait_status(lambda state: state['discovery_generation'] > generation and not state['discovering'], timeout=45)

    try:
        admin.login(password)
        original_config = admin.request('/api/config')
        fallback = admin.request('/api/status')['front_physical_address']
        physicals = [f'{port:X}.0.0.0' for port in (3, 2, 4) if f'{port:X}.0.0.0' != fallback]
        first, second = physicals[:2]
        for adapter in (args.front_adapter, args.back_adapter):
            info = harness.command(['cec-ctl', '-d', adapter])
            if 'cec-gpio' not in info:
                raise AssertionError(adapter + ' is not an isolated cec-gpio fixture adapter')
        configured = True
        harness.cec(args.front_adapter, '--clear', '--tv', '--phys-addr', '0.0.0.0',
                    '--osd-name', 'HarnessTV', '--vendor-id', '0x000001')
        source(first)

        def upload():
            request = urllib.request.Request(admin.base + '/api/firmware',
                data=args.firmware.read_bytes(), headers={'Content-Type': 'application/octet-stream',
                                                         'Authorization': 'Bearer ' + admin.token})
            with urllib.request.urlopen(request, context=admin.context, timeout=180) as response:
                if response.status != 200:
                    raise AssertionError('OTA upload failed')
            check('firmware OTA accepted', True)

        state = boot(upload if args.firmware else None)
        expect_physical(state, first)
        source(second)
        expect_physical(rescan(), first)
        check('rescan and unsolicited reports preserve startup choice', True)
        expect_physical(boot(), second)
        check('next boot reevaluates the connected source', True)

        harness.cec(args.back_adapter, '--clear', '--unregistered', '--phys-addr', second)
        state = boot()
        expect_physical(state, fallback)
        check('no responding back device uses configured fallback', state['front_physical_address'] == fallback)
        source(second)
        expect_physical(rescan(), fallback)
        check('fallback remains determined through later discovery', True)
        check('activities and configured default preserved', admin.request('/api/config') == original_config and
              admin.request('/api/status')['front_physical_address'] == fallback)
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        try:
            if configured:
                source(args.restore_physical)
                state = boot()
                if 'determined_front_physical_address' in state:
                    expect_physical(state, args.restore_physical)
        except Exception as error:
            report['passed'] = False
            report['restore_error'] = str(error)
            raise
        finally:
            write_private(args.output, report)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
