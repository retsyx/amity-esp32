#!/usr/bin/env python3
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

"""Check automatic pairing on an unpaired DUT using the existing Pi HID fixture.

Requires exclusive fixture use. Runs a real browser against the DUT, verifies
search timeout, rejected bonding and automatic encrypted HID pairing, then
removes only test-created bonds and restores the Pi's prior Pairable setting.
Credentials are read from a private JSON file and never included in output.
"""
import argparse
import json
import re
from pathlib import Path
import shlex
import subprocess
import time

from playwright.sync_api import sync_playwright, expect
from cec_web_activity import Admin, write_private


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', required=True)
    parser.add_argument('--credentials', type=Path, required=True)
    parser.add_argument('--host', default='pi@10.0.1.119')
    parser.add_argument('--output', type=Path, default=Path('test-results/ble-auto-pair-hardware.json'))
    args = parser.parse_args()
    password = json.loads(args.credentials.read_text())['admin_password']
    admin = Admin(args.url)
    admin.login(password)
    original = {k: admin.request('/api/' + k) for k in ('config', 'matter', 'remotes', 'status')}
    assert original['status']['hostname'] == 'amity-dut', 'Run against the authorized DUT'
    assert not original['remotes']['bonded'], 'Do not replace an existing user remote'
    root = '/home/pi/amity/esp32'
    fixture = root + '/tests/ble_hid_emulator.py'
    wheel = root + '/tests/dbus_next-0.2.3-py3-none-any.whl'
    report = {'passed': False, 'checks': [], 'states': [], 'http': admin.records}
    pid = None

    def ssh(command):
        return subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8',
            '-o', 'UserKnownHostsFile=test-results/harness_known_hosts', args.host, command],
            check=True, text=True, capture_output=True, timeout=20).stdout

    def check(name, passed):
        assert passed, name
        report['checks'].append(name)
        print('PASS ' + name, flush=True)

    def bonds():
        return {line.split()[1] for line in ssh('sudo -n bluetoothctl devices Bonded').splitlines()
                if line.startswith('Device ')}

    def wait(predicate, timeout=50):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            state = admin.request('/api/remotes')
            report['states'].append(state)
            if predicate(state):
                return state
            time.sleep(.3)
        raise AssertionError('Remote operation timed out')

    previous_bonds = bonds()
    previous_adapter = ssh('bluetoothctl show')
    fixture_name = next(line.split(': ', 1)[1] for line in previous_adapter.splitlines() if line.startswith('\tName: '))
    ssh('test ! -S ' + shlex.quote(fixture.replace('.py', '.sock')))
    try:
        with sync_playwright() as p:
            browser = p.chromium.launch(headless=True)
            try:
                page = browser.new_page(ignore_https_errors=True)
                page.goto(args.url)
                page.locator('#password').fill(password)
                page.locator('#password').press('Enter')
                page.locator('[data-tab=remotes]').click()
                page.locator('#toggleRemote').click()
                expect(page.locator('#remotes progress, #remoteProgress')).to_have_count(0)
                expect(page.locator('#remoteStatus')).to_have_text(re.compile(r'Looking for a nearby remote.*|Remote detected\. Bring it closer to Amity\.'))
                expect(page.locator('#toggleRemote')).to_be_disabled()
                check('live browser shows automatic search text without progress', True)
                wait(lambda s: not s['pairing'] and not s['scanning'] and not s['connecting'])
                expect(page.locator('#remotes progress, #remoteProgress')).to_have_count(0)
                expect(page.locator('#remoteStatus')).to_contain_text('No nearby remote found')
                check('empty search times out with retry feedback', True)

                # Detach only this fixture; retain its exact PID for cleanup.
                launch = ("import os,subprocess; os.umask(0o077); "
                    "f=open(" + repr(root + '/test-results/auto-pair-emulator.log') + ", 'wb'); "
                    "p=subprocess.Popen(['/usr/bin/python3'," + repr(fixture) + "], "
                    "env={**os.environ,'PYTHONPATH':" + repr(wheel) + "}, "
                    "stdin=subprocess.DEVNULL,stdout=f,stderr=subprocess.STDOUT,start_new_session=True); "
                    "print(p.pid)")
                pid = int(ssh(shlex.join(['sudo', '-n', 'python3', '-c', launch])).strip())
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    log = ssh('sudo -n cat ' + shlex.quote(root + '/test-results/auto-pair-emulator.log'))
                    if '"ready": true' in log:
                        break
                    time.sleep(.3)
                else:
                    raise AssertionError('BLE emulator failed to start')

                # A connectable advertisement is not proof of bond acceptance.
                ssh('sudo -n bluetoothctl pairable off')
                page.locator('#toggleRemote').click()
                rejected = wait(lambda s: s['pairing'] and s['last_error'] != 0)
                check('rejecting peer is skipped without a saved remote bond', not rejected['bonded'])
                wait(lambda s: not s['pairing'] and not s['connecting'] and not s['scanning'])
                expect(page.locator('#remotes progress, #remoteProgress')).to_have_count(0)
                expect(page.locator('#remoteStatus')).to_contain_text('Pairing failed')

                ssh('sudo -n bluetoothctl pairable on')
                page.locator('#toggleRemote').click()
                expect(page.locator('#remotes progress, #remoteProgress')).to_have_count(0)
                paired = wait(lambda s: s['bonded'] and s['connected'] and not s['pairing'])
                check('first nearby remote automatically bonds and connects', paired['name'] == fixture_name)
                check('battery is read over the encrypted HID connection', paired['battery'] == 87)
                expect(page.locator('#remoteStatus')).to_have_text('Paired')
                expect(page.locator('#remotes progress, #remoteProgress')).to_have_count(0)
                expect(page.locator('#toggleRemote')).to_have_text('Unpair remote')
                check('live browser completes pairing without a candidate list',
                      page.locator('#remotes button').count() == 1)
            finally:
                browser.close()
    finally:
        try:
            admin.request('/api/remotes/unpair', {})
            wait(lambda s: not s['bonded'] and not s['connected'] and not s['pairing'])
            for address in bonds() - previous_bonds:
                ssh(shlex.join(['sudo', '-n', 'bluetoothctl', 'remove', address]))
        finally:
            if pid is not None:
                stop = ("import os,signal; from pathlib import Path; "
                    "p=" + str(pid) + "; "
                    "assert " + repr(fixture.encode()) + " in Path(f'/proc/{p}/cmdline').read_bytes(); "
                    "os.kill(p,signal.SIGTERM)")
                ssh(shlex.join(['sudo', '-n', 'python3', '-c', stop]))
                time.sleep(1)
            check('Pi original bonds retained', bonds() == previous_bonds)
            was_pairable = '\tPairable: yes' in previous_adapter
            check('Pi Pairable setting restored', ('\tPairable: yes' in ssh('bluetoothctl show')) == was_pairable)
            check('DUT activities and Matter fabrics retained',
                admin.request('/api/config') == original['config'] and
                admin.request('/api/matter')['fabrics'] == original['matter']['fabrics'])
            write_private(args.output, report)
    report['passed'] = True
    write_private(args.output, report)


if __name__ == '__main__':
    main()
