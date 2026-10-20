#!/usr/bin/env python3
"""Validate the local octopus adapter on the isolated two-line DUT fixture.

Uses private saved admin credentials. Preserves the original portable config;
never erases pairings, origin, factory data or Wi-Fi credentials. Wire captures
establish CEC behavior only, not HDMI picture/audio or ten-line qualification.
"""
import argparse
import shlex
import subprocess
import time
import json
import urllib.request
from pathlib import Path

from cec_hardware import Harness, raw_frames
from cec_web_activity import Admin, read_password, write_private


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', required=True)
    parser.add_argument('--credentials', type=Path, required=True)
    parser.add_argument('--host', required=True)
    parser.add_argument('--firmware', type=Path, help='Install this already-built image through authenticated OTA first')
    parser.add_argument('--output', type=Path, default=Path('test-results/octopus-local-hardware.json'))
    args = parser.parse_args()
    admin = Admin(args.url)
    harness = Harness(args.host)
    report = {'passed': False, 'checks': [], 'http': admin.records, 'cec': harness.records}
    original = None
    monitors = []

    def check(name, passed):
        if not passed:
            raise AssertionError(name)
        report['checks'].append(name)
        print('PASS', name, flush=True)

    try:
        password = read_password(args.credentials)
        admin.login(password)
        original = admin.request('/api/config')
        initial = admin.request('/api/v2/system')
        if args.firmware:
            previous_boot = admin.request('/api/setup')['boot_id']
            request = urllib.request.Request(args.url.rstrip('/') + '/api/firmware',
                data=args.firmware.read_bytes(), headers={'Authorization': 'Bearer ' + admin.token,
                                                         'Content-Type': 'application/octet-stream'})
            with urllib.request.urlopen(request, context=admin.context, timeout=300) as response:
                json.load(response)
            deadline = time.monotonic() + 90
            while time.monotonic() < deadline:
                time.sleep(.5)
                try:
                    setup = admin.request('/api/setup')
                    if setup['boot_id'] != previous_boot and setup['ready']:
                        break
                except OSError:
                    pass
            else:
                raise AssertionError('OTA image did not complete a fresh boot')
            admin.login(password)
            updated = admin.request('/api/v2/system')
            check('OTA retains origin and singleton identity', all(initial[k] == updated[k] for k in ('octopus_id', 'consortium_id')))
            check('OTA preserves portable config', admin.request('/api/config') == original)
            initial = updated
        check('honest single-board capabilities', initial['mode'] == 'legacy_local' and
              initial['capabilities']['supported_tentacles'] == 1 and
              initial['capabilities']['supported_octopuses'] == 1 and
              initial['capabilities']['consortium_enrollment'] is False)
        check('two CEC lines use one timer', initial['cec_runtime']['active_ports'] == 2 and
              initial['cec_runtime']['timer_instances'] == 1)
        fixture = {'version': 1, 'activities': [
            {'name': 'Octopus validation', 'display': 'HarnessTV', 'source': 'HarnessSource', 'audio': ''}]}
        admin.request('/api/config', fixture)
        scan = admin.request('/api/status')['discovery_generation']
        admin.request('/api/discover', {})
        state = admin.wait_status(lambda s: s['discovery_generation'] != scan and not s['discovering'], timeout=45)
        check('fixture equipment identified', {'HarnessTV', 'HarnessSource'} <= {d['name'] for d in state['devices']})
        admin.activity(0)
        state = admin.wait_status(lambda s: not s['activity_settling'], timeout=20)
        generation = state['activity_generation'] if 'activity_generation' in state else None
        for adapter in ('/dev/cec0', '/dev/cec1'):
            command = shlex.join(['sudo', '-n', 'cec-ctl', '-d', adapter, '-s', '-r', '-v', '-M', '--monitor-time', '25'])
            process = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=no',
                                        '-o', 'UserKnownHostsFile=/dev/null', args.host, command],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            monitors.append((adapter, process))
        time.sleep(.5)
        fixture['activities'][0]['name'] = 'Renamed validation'
        admin.request('/api/config', fixture)
        state = admin.wait_status(lambda s: s['current_activity'] == 0)
        if generation is not None:
            check('rename preserves input generation', state['activity_generation'] == generation)
        scan = state['discovery_generation']
        admin.request('/api/discover', {})
        admin.wait_status(lambda s: s['discovery_generation'] != scan and not s['discovering'], timeout=45)
        fixture['activities'][0]['display'] = ''
        admin.request('/api/config', fixture)
        admin.wait_status(lambda s: s['current_activity'] == -1)
        # Inject an unsolicited source activation while configured but idle.
        # It cannot authorize standby or routing on either protected side.
        harness.cec('/dev/cec1', '--active-source', 'phys-addr=1.0.0.0')
        captures = {}
        forbidden = {0x04, 0x0d, 0x36, 0x44, 0x45, 0x80, 0x81, 0x82, 0x86}
        for adapter, process in monitors:
            stdout, stderr = process.communicate(timeout=32)
            if process.returncode:
                raise RuntimeError(stderr)
            captures[adapter] = stdout
            initiator = state['front_address'] if adapter.endswith('0') else state['back_address']
            emitted = [f for f in raw_frames(stdout) if len(f) > 1 and f[0] >> 4 == initiator]
            check('no save/scan/source-triggered actuation on ' + adapter,
                  not any(f[1] in forbidden for f in emitted))
        report['captures'] = captures
        check('isolated source activation stimulus captured',
              bytes([0x4f, 0x82, 0x10, 0]) in raw_frames(captures['/dev/cec1']))
        report['runtime_under_api_and_scan_load'] = admin.request('/api/v2/system')['cec_runtime']
        previous_boot = admin.request('/api/setup')['boot_id']
        admin.request('/api/reboot', {})
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            time.sleep(.5)
            try:
                setup = admin.request('/api/setup')
                if setup['boot_id'] != previous_boot and setup['ready']:
                    break
            except OSError:
                pass
        else:
            raise AssertionError('DUT restart did not finish')
        admin.login(password)
        final = admin.request('/api/v2/system')
        check('origin and singleton identity survive reboot', all(initial[k] == final[k] for k in ('octopus_id', 'consortium_id')))
        check('reboot preserves config without replaying an activity',
              admin.request('/api/config') == fixture and admin.request('/api/status')['current_activity'] == -1)
        report['runtime'] = final['cec_runtime']
        report['passed'] = True
        return 0
    except Exception as error:
        report['error'] = str(error)
        report['last_status'] = admin.last_status
        print('FAIL', error, flush=True)
        return 1
    finally:
        for _, process in monitors:
            if process.poll() is None:
                process.terminate()
                process.communicate(timeout=5)
        if original is not None:
            try:
                admin.request('/api/config', original)
            except Exception:
                report['restore_failed'] = True
        write_private(args.output, report)


if __name__ == '__main__':
    raise SystemExit(main())
