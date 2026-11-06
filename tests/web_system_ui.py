#!/usr/bin/env python3
"""Check the baseline interface and main/added-device management list."""
import argparse
import hashlib
import json
import os
import re
from copy import deepcopy
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Event, Thread
from playwright.sync_api import sync_playwright, expect

ROOT = Path(__file__).resolve().parents[1]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--readiness-only', action='store_true',
                        help='check initial activity readiness and the first Start without later editing cases')
    args = parser.parse_args(argv)
    # Exact approved main presentation at aa9395aaecdc861e1d4cce040079a5ac6121e67d,
    # with HDMI-CEC and hostname controls moved to per-device settings. Firmware
    # upload/progress now covers the whole system and has its own browser tests.
    # The activity editor's save-before-start hint was explicitly removed.
    # Keep the remaining presentation unchanged without requiring that Git object.
    baseline_sha256 = '429791583d8daf53a2e369d2f7a65a7b648faaf7d8c1cacc45e066e9c71e4c07'
    current = (ROOT/'components/web_admin/assets/index.html').read_text()
    current = current.replace('<script src="/system.js" defer></script>', '').replace('<button data-tab="amityDevices">Amity devices</button>', '')
    current = re.sub(r'<section id="amityDevices".*?</section>', '', current)
    current = re.sub(r'<section id="firmwareUpdate".*?</section>', '', current)
    current = re.sub(r'<div class="setting"><h3>Update Firmware</h3>.*?</div>', '', current)
    assert hashlib.sha256(current.encode()).hexdigest() == baseline_sha256, 'Changes outside device management and system firmware updates'
    assert 'changeFrontPhysicalAddress' not in current and 'frontPhysicalAddress' not in current
    assert 'changeHostname' not in current and 'id="hostname"' not in current
    aid, second, controller, member = (f'{n:032x}' for n in (7, 8, 2, 3))
    def choice(value, title, location, roles):
        return dict(value='component:'+f'{value:032x}', title=title, location=location, roles=roles)
    choices = [choice(10, 'Display', 'amity-living-room', ['display', 'audio']),
               choice(11, 'Apple TV', 'amity-living-room', ['source']),
               choice(12, 'Apple TV', 'amity-bedroom · Adapter 4', ['source'])]
    activities = [dict(activity_id=aid, name='Watch TV', display=choices[0]['value'], source=choices[1]['value'], audio=choices[0]['value']),
                  dict(activity_id=second, name='Bedroom source', display=choices[0]['value'], source=choices[2]['value'], audio=choices[0]['value'])]
    editor = dict(version=1, expected_config_revision='1', expected_presentation_revision='1', activities=activities, device_choices=choices)
    status = dict(version='0.1.73', hostname='amity-living-room', front_physical_address='1.0.0.0', hardware='Amity', free_heap=200000,
                  wifi_connected=True, wifi_rssi=-40, wifi_channel=6, ready=True, discovering=False, discovery_generation=1,
                  current_activity=-1, front_address=4, back_address=0, tx_errors=0, rx_dropped=0,
                  expected_state_revision='0', expected_boot_id='a'*32, devices=[], device_choices=choices)
    status['devices'] = [dict(name=c['value'], title=c['title'], location=c['location'], bus='front' if i == 0 else 'back', type=0 if i == 0 else 4, address=0 if i == 0 else 4, physical_address=0 if i == 0 else 4096) for i, c in enumerate(choices)]
    configuration = dict(version=3, controller_octopus_id=controller, config_revision='1', presentation_revision='1',
                         octopuses=[dict(octopus_id=member, label='Bedroom Amity', supported_tentacles=4), dict(octopus_id=controller, label='Living room Amity', supported_tentacles=1)],
                         activities=[], zones=[], external_slots=[dict(slot=5, activity_id=aid)])
    system = dict(octopus_id=controller, hostname='amity-living-room',
                  named_devices=[dict(octopus_id=controller, hostname='amity-living-room'),
                                 dict(octopus_id=member, hostname='amity-bedroom')],
                  membership=dict(membership_revision='1', peers=[dict(octopus_id=member, hostname='amity-bedroom', enrolled=True, ready=True, boot_id='b'*32)]))
    remote = dict(bonded=False, ready=True, devices=[], boot_id='b'*32, remote_generation='1')
    fixture = dict(posts=[], operations={}, next_id=100, scans=0, fail_save=False, fail_add=True, config=configuration, remove_mode='scheduled', remove_release=False, admission=dict(scan_id=None, phase='idle', error='', requires_setup_code=False, candidates=[], candidate_id=None))
    errors, checks = [], []
    initial_status_requested, initial_status_ready = Event(), Event()
    def passed(value):
        checks.append(value)
        print('PASS', value, flush=True)

    def purge_removal(data):
        removed = set(data['affected_activity_ids'])
        owned = {component['component_id'] for component in fixture['config'].get('components', [])
                 if any(name['octopus_id'] == member for name in component.get('qualified_names', []))}
        fixture['config']['octopuses'] = [owner for owner in fixture['config']['octopuses'] if owner['octopus_id'] != member]
        fixture['config']['components'] = [component for component in fixture['config'].get('components', []) if component['component_id'] not in owned]
        fixture['config']['activities'] = [activity for activity in fixture['config']['activities'] if activity['activity_id'] not in removed]
        editor['activities'] = [activity for activity in editor['activities'] if activity['activity_id'] not in removed]
        system['membership']['peers'] = []
        system['named_devices'] = [device for device in system['named_devices'] if device['octopus_id'] != member]

    class Handler(SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=str(ROOT/'components/web_admin/assets'), **kwargs)
        def log_message(self, *args):
            pass
        def reply(self, value, code=200):
            data = json.dumps(value).encode()
            self.send_response(code)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        def do_GET(self):
            if self.path == '/api/activity-editor/status' and not initial_status_ready.is_set():
                initial_status_requested.set()
                if not initial_status_ready.wait(timeout=30):
                    return self.reply(dict(error='Initial status fixture was not released'), 503)
            values = {'/api/setup': dict(required=False), '/api/activity-editor': editor,
                      '/api/activity-editor/status': status, '/api/config': fixture['config'], '/api/system': system,
                      '/api/remotes': remote, '/api/matter': dict(fabrics=[]), '/api/homekit': dict(available=False),
                      '/api/membership/admission': fixture['admission']}
            if self.path in values:
                return self.reply(deepcopy(values[self.path]))
            if self.path.startswith('/api/operations/'):
                operation = fixture['operations'][self.path.rsplit('/', 1)[-1]]
                operation['polls'] += 1
                if operation['kind'] == 'remove':
                    if fixture['remove_mode'] == 'poll_error':
                        purge_removal(operation['removal'])
                        return self.reply(dict(error='The removal result was lost'), 503)
                    if not fixture['remove_release']:
                        return self.reply(dict(operation_id=operation['id'], stage='admitted'))
                    purge_removal(operation['removal'])
                    return self.reply(dict(operation_id=operation['id'], stage='complete', reset=operation['reset'],
                                           message='Reset was not confirmed. Hold its physical button for at least 10 seconds to factory reset it before adding it again.'))
                if operation['polls'] == 1:
                    return self.reply(dict(operation_id=operation['id'], stage='stopping'))
                if 'activity_id' in operation:
                    status['current_activity'] = next((i for i, a in enumerate(editor['activities']) if a['activity_id'] == operation['activity_id']), -1)
                    status['expected_state_revision'] = str(int(status['expected_state_revision'])+1)
                return self.reply(dict(operation_id=operation['id'], stage='applied' if operation['kind'] == 'config' else 'complete', **({'unresolved':operation['unresolved']} if 'unresolved' in operation else {})))
            return super().do_GET()
        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers.get('Content-Length', '0'))))
            fixture['posts'].append((self.path, deepcopy(body)))
            if self.path == '/api/login':
                return self.reply(dict(token='fixture-session'))
            if self.path == '/api/activity-editor':
                assert body['version'] == 1
                assert body['expected_config_revision'] == editor['expected_config_revision']
                assert body['expected_presentation_revision'] == editor['expected_presentation_revision']
                if fixture['fail_save']:
                    return self.reply(dict(error='Activities changed. Reload before saving'), 409)
                for row in body['activities']:
                    for role in ('display', 'source', 'audio'):
                        assert not row[role] or row[role].startswith('component:'), row
                    if 'activity_id' not in row:
                        row['activity_id'] = f'{fixture["next_id"]:032x}'
                        fixture['next_id'] += 1
                editor['activities'] = body['activities']
                editor['expected_config_revision'] = str(int(editor['expected_config_revision'])+1)
                fixture['config']['config_revision'] = editor['expected_config_revision']
                kind = 'config'
            elif self.path == '/api/activity-editor/activity':
                assert 'index' not in body and body['expected_boot_id'] == status['expected_boot_id']
                assert body['expected_state_revision'] == status['expected_state_revision']
                assert body['expected_config_revision'] == editor['expected_config_revision']
                kind = 'activity'
            elif self.path == '/api/activity-editor/discover':
                fixture['scans'] += 1
                status['discovery_generation'] += 1
                return self.reply(dict(stage='complete'))
            elif self.path == '/api/activity-editor/restore':
                assert body['confirm'] is True and body['previous_controller_isolated'] is True
                assert body['expected_boot_id'] == status['expected_boot_id']
                assert body['expected_config_revision'] == editor['expected_config_revision']
                assert body['expected_presentation_revision'] == editor['expected_presentation_revision']
                assert body['configuration']['controller_octopus_id'] == 'f'*32
                kind = 'config'
            elif self.path == '/api/membership/scan':
                assert set(body) == {'request_id'}
                fixture['admission'].update(scan_id=body['request_id'], phase='found', error='', candidates=[dict(token='nearby-token', label='Amity A1B2')], candidate_id=None)
                return self.reply(dict(operation_id=body['request_id'], stage='complete'))
            elif self.path == '/api/membership/cancel':
                assert set(body) == {'request_id', 'scan_id'} and body['scan_id'] == fixture['admission']['scan_id']
                fixture['admission']['phase'] = 'cancelled'
                return self.reply(dict(operation_id=body['request_id'], stage='complete'))
            elif self.path == '/api/membership/add':
                assert set(body) == {'request_id', 'scan_id', 'candidate_token', 'setup_code', 'name'}
                assert body['scan_id'] == fixture['admission']['scan_id'] and body['candidate_token'] == 'nearby-token'
                assert fixture['admission']['requires_setup_code'] and body['setup_code'] == '1234-567-8901'
                assert re.fullmatch(r'[a-z0-9](?:[a-z0-9-]{0,30}[a-z0-9])?', body['name'].lower())
                assert body['name'] in ('a', 'n' * 32, 'amity-office')
                if fixture['fail_add']:
                    return self.reply(dict(error='The setup code could not be verified'), 409)
                kind = 'add'
            elif self.path == '/api/membership/remove':
                assert set(body) == {'request_id', 'octopus_id', 'confirm', 'expected_membership_revision', 'expected_target_boot_id', 'expected_config_revision', 'expected_presentation_revision', 'affected_activity_ids'}
                assert body['octopus_id'] == member and body['confirm'] is True
                assert re.fullmatch('[0-9a-f]{32}', body['request_id'])
                assert body['expected_membership_revision'] == '1'
                assert body['expected_config_revision'] == saved_remove_config['config_revision']
                assert body['expected_presentation_revision'] == saved_remove_config['presentation_revision']
                assert body['affected_activity_ids'] == [row['activity_id'] for row in saved_remove_config['activities'] if row.get('label', '').startswith('Uses ')]
                assert body['expected_target_boot_id'] == (None if fixture['remove_mode'] == 'offline' else 'b'*32)
                if fixture['remove_mode'] in ('stale_revision', 'stale_boot', 'stale_configuration'):
                    return self.reply(dict(error='The Amity changed. Reopen Remove and review its current state.'), 409)
                if fixture['remove_mode'] == 'unsupported':
                    return self.reply(dict(error='Update this Amity before removing it so it can factory reset safely.'), 409)
                if fixture['remove_mode'] == 'post_error':
                    purge_removal(body)
                    return self.reply(dict(error='The removal response was lost'), 503)
                kind = 'remove'
            elif self.path in ('/api/remotes/pair', '/api/remotes/unpair'):
                assert body['expected_boot_id'] == remote['boot_id']
                assert body['expected_remote_generation'] == remote['remote_generation']
                assert re.fullmatch('[0-9a-f]{32}', body['request_id'])
                remote['remote_generation'] = str(int(remote['remote_generation'])+1)
                return self.reply(dict(ok=True))
            else:
                return self.reply(dict(error='Unknown fixture endpoint'), 404)
            request = body['request_id']
            op = dict(id=request, kind=kind, polls=0)
            if kind == 'remove':
                op['reset'] = fixture['remove_mode']
                op['removal'] = deepcopy(body)
            if self.path.endswith('/restore'):
                op['unresolved'] = ['Configuration restored.', 'Re-pair the remote and re-enroll added Amity devices before repairing activities.']
            if kind == 'activity':
                op['activity_id'] = body['activity_id']
            fixture['operations'][request] = op
            return self.reply(dict(operation_id=request, stage='configuring' if kind == 'config' else 'admitted'))

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    Thread(target=server.serve_forever, daemon=True).start()
    try:
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(
                headless=True, executable_path=os.environ.get('PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH'))
            page = browser.new_page(viewport=dict(width=430, height=900))
            page.on('pageerror', lambda e: errors.append(str(e)))
            page.goto(f'http://127.0.0.1:{server.server_port}/')
            page.locator('#password').fill('fixture')
            page.locator('#password').press('Enter')
            expect(page.locator('#admin')).to_be_visible()
            assert initial_status_requested.wait(timeout=5), 'The UI did not request its initial playback status'
            expect(page.locator('#activityList [data-start]').first).to_be_disabled()
            assert not [p for p, _ in fixture['posts'] if p.endswith('/activity')]
            initial_status_ready.set()
            expect(page.locator('#activityList [data-start]').first).to_be_enabled()
            passed('Start waits for its first playback snapshot and accepts revision zero')
            expect(page.locator('nav button')).to_have_text(['Activities', 'Remote', 'Matter', 'HomeKit', 'Amity devices', 'Advanced'])
            expect(page.locator('#systemActivities, #equipment, #settings')).to_have_count(0)
            passed('original navigation and activity list plus exactly one added-device tab')
            assert not [p for p, _ in fixture['posts'] if p != '/api/login']
            passed('opening the interface performs no configuration or playback action')
            page.locator('#activityList [data-start]').first.click()
            expect(page.locator('#activityList .active')).to_have_count(1)
            assert [b for p, b in fixture['posts'] if p.endswith('/activity')][-1]['activity_id'] == aid
            assert fixture['operations'][[b for p, b in fixture['posts'] if p.endswith('/activity')][-1]['request_id']]['polls'] >= 2
            passed('Start uses stable identity and waits through stopping before completion')
            if args.readiness_only:
                assert not errors, errors
                passed('no browser script errors')
                browser.close()
                return
            page.locator('#discover').click()
            expect(page.locator('#deviceList .device')).to_have_count(3)
            expect(page.locator('#deviceList')).to_contain_text('amity-living-room')
            expect(page.locator('#deviceList')).to_contain_text('amity-bedroom · Adapter 4')
            assert sorted(page.locator('#deviceList .device-location').all_text_contents()) == [
                'amity-bedroom · Adapter 4', 'amity-living-room', 'amity-living-room']
            expect(page.locator('#deviceList')).not_to_contain_text('amity-living-room · Adapter')
            assert not re.search(r'HDMI adapter|screen connection|source connection', page.locator('#deviceList').inner_text(), re.I)
            assert not re.search(r'\b(octopus|tentacle|consortium)\b', page.locator('body').inner_text(), re.I)
            page.screenshot(path=str(ROOT/'test-results/controller-locations-ui-0.1.73.png'), full_page=True)
            passed('equipment labels keep owning hostnames and show adapter numbers only for owners with multiple adapters')
            page.locator('#activityList article').first.get_by_role('button', name='Edit', exact=True).click()
            labels = page.locator('#activityFields label').all_text_contents()
            assert [x.split('\n')[0] for x in labels] == ['Name', 'Display', 'Source', 'Audio', 'Switch Device', 'Input']
            source = page.locator('#activityFields input').nth(2)
            source.fill('Apple TV · amity-bedroom · Adapter 4')
            page.locator('#activityForm button[type=submit]').click()
            count = len([1 for p, _ in fixture['posts'] if p.endswith('/activity')])
            page.locator('#saveActivities').click()
            expect(page.locator('#saveActivities')).to_be_hidden()
            assert editor['activities'][0]['source'] == choices[2]['value'] and editor['activities'][0]['activity_id'] == aid
            assert len([1 for p, _ in fixture['posts'] if p.endswith('/activity')]) == count
            assert fixture['config']['external_slots'] == [dict(slot=5, activity_id=aid)]
            passed('original editor fields save exact scoped equipment without playing or changing selectors')
            page.locator('#activityList article').first.get_by_role('button', name='Edit', exact=True).click()
            page.locator('#activityFields input').nth(0).fill('Renamed TV')
            page.locator('#editorStart').click()
            expect(page.locator('#editorStart')).to_have_text('Repair')
            page.locator('#cancelActivity').click()
            assert editor['activities'][0]['name'] == 'Renamed TV'
            assert [b for p, b in fixture['posts'] if p.endswith('/activity')][-1]['activity_id'] == aid
            passed('Save and start retains the original control and stable activity identity')
            page.locator('#addActivity').click()
            for index, value in enumerate(['New source', 'Display · amity-living-room', 'Apple TV · amity-bedroom · Adapter 4', 'Display · amity-living-room']):
                page.locator('#activityFields input').nth(index).fill(value)
            page.locator('#activityForm button[type=submit]').click()
            page.locator('#saveActivities').click()
            expect(page.locator('#saveActivities')).to_be_hidden()
            created = editor['activities'][-1]['activity_id']
            page.locator('#activityList article').last.get_by_role('button', name='Edit', exact=True).click()
            page.locator('#activityFields input').nth(0).fill('Renamed new source')
            page.locator('#activityForm button[type=submit]').click()
            page.locator('#saveActivities').click()
            expect(page.locator('#saveActivities')).to_be_hidden()
            assert editor['activities'][-1]['activity_id'] == created
            assert editor['activities'][-1]['name'] == 'Renamed new source'
            passed('new activity identity is hydrated after save and retained by a later edit')
            page.locator('[data-tab=remotes]').click()
            expect(page.locator('#toggleRemote')).to_have_text('Pair remote')
            with page.expect_response(lambda r: r.url.endswith('/api/remotes/pair')):
                page.locator('#toggleRemote').click()
            assert any(p == '/api/remotes/pair' for p, _ in fixture['posts'])
            expect(page.get_by_text('Change screen or owner', exact=True)).to_have_count(0)
            passed('original Remote tab pairs locally without owner or screen assignment UI')
            remote['bonded'] = True
            page.evaluate('refresh()')
            expect(page.locator('#toggleRemote')).to_have_text('Unpair remote')
            page.once('dialog', lambda dialog: dialog.accept())
            with page.expect_response(lambda r: r.url.endswith('/api/remotes/unpair')):
                page.locator('#toggleRemote').click()
            pair = [b for p, b in fixture['posts'] if p.endswith('/remotes/pair')][-1]
            unpair = [b for p, b in fixture['posts'] if p.endswith('/remotes/unpair')][-1]
            assert pair['request_id'] != unpair['request_id'] and unpair['expected_remote_generation'] == '2'
            passed('local Pair and Unpair retain the original controls with fresh boot and one-use generation guards')
            page.locator('[data-tab=amityDevices]').click()
            rows = page.locator('#managedAmityList > article')
            expect(rows).to_have_count(2)
            expect(rows.nth(0)).to_contain_text('amity-living-room')
            expect(rows.nth(0).locator('h3')).to_have_text('amity-living-room (this device)')
            expect(rows.nth(1)).to_contain_text('amity-bedroom')
            expect(rows.nth(1).locator('h3')).not_to_contain_text('(this device)')
            for action in ('Remove', 'Replace', 'Update firmware'):
                expect(rows.nth(0).get_by_role('button', name=action, exact=True)).to_have_count(0)
            expect(rows.nth(1).get_by_role('button', name='Remove', exact=True)).to_be_visible()
            for width in (320, 430, 1280):
                page.set_viewport_size(dict(width=width, height=1000))
                for index, names in ((0, ('Settings',)), (1, ('Settings', 'Remove', 'Replace'))):
                    row = rows.nth(index)
                    # Read one DOM snapshot so the normal polling refresh
                    # cannot replace the card between rectangle reads.
                    boxes = row.evaluate('''(row,names)=>{
                        const rect=node=>{const r=node.getBoundingClientRect();return {x:r.x,y:r.y,width:r.width,height:r.height};};
                        const controls=[...row.querySelectorAll('button')];
                        return {heading:rect(row.querySelector('h3')),header:rect(row.querySelector('.amity-device-header')),
                            buttons:names.map(name=>rect(controls.find(button=>button.textContent===name)))};
                    }''', list(names))
                    heading, buttons, header = boxes['heading'], boxes['buttons'], boxes['header']
                    assert heading['x'] + heading['width'] <= buttons[0]['x']
                    assert all(abs((b['y'] + b['height']/2) - (heading['y'] + heading['height']/2)) < 1 for b in buttons)
                    assert abs(buttons[-1]['x'] + buttons[-1]['width'] - header['x'] - header['width']) < 1
                    assert row.evaluate('(e)=>e.scrollWidth<=e.clientWidth+1')
            page.set_viewport_size(dict(width=430, height=1000))
            passed('inline headings and device actions remain aligned at 320, 430 and 1280 pixels')
            expect(page.locator('#managedAmityList')).not_to_contain_text('Controlled by the original Amity')
            expect(page.locator('#managedAmityList')).not_to_contain_text('HDMI adapters')
            expect(page.get_by_text('Manage added devices from this original Amity. They have no separate interface or remote.', exact=True)).to_have_count(0)
            expect(page.get_by_role('button', name='Move coordinator')).to_have_count(0)
            page.screenshot(path=str(ROOT/'test-results/controller-managed-devices-ui-0.1.73.png'), full_page=True)
            expect(page.locator('#managedAmityList')).not_to_contain_text('Living room Amity')
            expect(page.locator('#managedAmityList')).not_to_contain_text('Bedroom Amity')
            expect(page.get_by_role('button', name='Location', exact=True)).to_have_count(0)
            saved_peers = system['membership']['peers']
            saved_names = system['named_devices']
            system['membership']['peers'] = [dict(octopus_id=member, enrolled=True, ready=False)]
            try:
                page.evaluate('amityInterface.refresh()')
                expect(rows.nth(1)).to_contain_text('amity-bedroom')
                expect(rows.nth(1)).to_contain_text('Not responding')
                system['named_devices'] = [n for n in saved_names if n['octopus_id'] != member]
                page.evaluate('amityInterface.refresh()')
                expect(rows.nth(1)).to_contain_text('Name unavailable')
                expect(rows.nth(1)).not_to_contain_text('Bedroom Amity')
            finally:
                system['membership']['peers'] = saved_peers
                system['named_devices'] = saved_names
                page.evaluate('amityInterface.refresh()')
            passed('management uses canonical live/cached hostnames, never old labels, with this main first and no member controls')
            saved_boards = fixture['config']['octopuses']
            saved_peers = system['membership']['peers']
            fixture['config']['octopuses'] = [b for b in saved_boards if b['octopus_id'] == controller]
            system['membership']['peers'] = []
            system['named_devices'] = [device for device in saved_names if device['octopus_id'] == controller]
            try:
                page.evaluate('amityInterface.refresh()')
                expect(rows).to_have_count(1)
                expect(rows.first).to_contain_text('amity-living-room')
                expect(rows.first.locator('h3')).to_have_text('amity-living-room (this device)')
                for action in ('Remove', 'Replace', 'Update firmware'):
                    expect(rows.first.get_by_role('button', name=action, exact=True)).to_have_count(0)
                passed('a system with no added devices still lists this main by its actual hostname')
            finally:
                fixture['config']['octopuses'] = saved_boards
                system['membership']['peers'] = saved_peers
                system['named_devices'] = saved_names
                page.evaluate('amityInterface.refresh()')
            expect(rows).to_have_count(2)
            fixture['admission']['requires_setup_code'] = True
            page.locator('#addAmity').click()
            dialog = page.locator('dialog:visible')
            expect(dialog).to_contain_text('Hold the Reset button on the new Amity for 3 seconds.')
            expect(dialog.get_by_label('Matter setup code')).to_be_visible()
            secret = '1234-567-8901'
            name = dialog.get_by_label('Name', exact=True)
            expect(name).to_have_attribute('required', '')
            expect(name).to_have_attribute('maxlength', '32')
            expect(dialog.get_by_label('Location', exact=True)).to_have_count(0)
            before_add = len([p for p, _ in fixture['posts'] if p == '/api/membership/add'])
            for invalid in ('', 'bad name', '-bad', 'bad-', 'amity.room', 'café'):
                name.fill(invalid)
                dialog.get_by_label('Matter setup code').fill(secret)
                dialog.get_by_role('button', name='Add', exact=True).click()
                assert len([p for p, _ in fixture['posts'] if p == '/api/membership/add']) == before_add
                expect(dialog).to_be_visible()
            for entered, canonical in (('a', 'a'), ('n' * 32, 'n' * 32), ('  AMITY-OFFICE  ', 'amity-office')):
                name.fill(entered)
                dialog.get_by_label('Matter setup code').fill(secret)
                with page.expect_response(lambda r: r.url.endswith('/api/membership/add')):
                    dialog.get_by_role('button', name='Add', exact=True).click()
                expect(page.locator('#message')).to_have_text('The setup code could not be verified')
                expect(dialog.get_by_role('button', name='Add', exact=True)).to_be_disabled()
                assert [body for path, body in fixture['posts'] if path == '/api/membership/add'][-1]['name'] == canonical
                expect(dialog.get_by_label('Matter setup code')).to_have_value('')
                dialog.get_by_label('Matter setup code').fill(secret)
                expect(dialog.get_by_role('button', name='Add', exact=True)).to_be_enabled()
            assert secret not in page.evaluate('JSON.stringify({...localStorage,...sessionStorage})')
            with page.expect_response(lambda r: r.url.endswith('/api/membership/cancel')):
                dialog.get_by_role('button', name='Cancel').click()
            passed('Add scans first, requires a valid hostname, uses only strict printed Matter codes, clears them after failure and cancels the exact scan')
            # All destructive responses below are synthetic localhost fixture
            # outcomes. No device, pairing, network or reset API is contacted.
            saved_config = deepcopy(fixture['config'])
            saved_editor = deepcopy(editor)
            saved_peers = deepcopy(system['membership']['peers'])
            saved_remove_config = deepcopy(saved_config)
            saved_remove_config['components'] = [dict(component_id=f'{n:032x}', qualified_names=[dict(octopus_id=member)]) for n in range(31, 35)]
            saved_remove_config['activities'] = [dict(activity_id=f'{n+40:032x}', label='Uses '+role, **{role:f'{n:032x}'})
                                                for n, role in zip(range(31, 35), ('display', 'source', 'audio', 'switch_device'))]
            saved_remove_config['activities'].append(dict(activity_id='f'*32, label='Unrelated local activity'))
            fixture['config'] = deepcopy(saved_remove_config)
            before_remove = len(fixture['posts'])
            before_scans = fixture['scans']
            rows.nth(1).get_by_role('button', name='Remove', exact=True).click()
            dialog = page.locator('dialog:visible')
            expect(dialog).to_contain_text('Removing this Amity deletes its saved equipment and settings.')
            expect(dialog).to_contain_text('If it responds, it will also factory reset.')
            expect(dialog).to_contain_text('physical button before adding it again')
            expect(dialog).to_contain_text('The following activities will be deleted if you confirm removal:')
            expect(dialog.locator('li')).to_have_text(['Uses display', 'Uses source', 'Uses audio', 'Uses switch_device'])
            expect(dialog).not_to_contain_text('Unrelated local activity')
            dialog.get_by_role('button', name='Cancel', exact=True).click()
            assert len(fixture['posts']) == before_remove and fixture['scans'] == before_scans
            assert system['membership']['peers'] == saved_peers and fixture['config'] == saved_remove_config
            passed('Remove lists every affected role, warns of activity deletion and leaves Cancel inert')
            for mode in ('scheduled', 'offline', 'unconfirmed', 'unsupported', 'post_error', 'poll_error', 'stale_revision', 'stale_boot', 'stale_configuration'):
                fixture['remove_mode'] = mode
                fixture['remove_release'] = False
                fixture['config'] = deepcopy(saved_remove_config)
                editor.clear()
                editor.update(deepcopy(saved_editor))
                system['membership']['peers'] = deepcopy(saved_peers)
                system['membership']['membership_revision'] = '1'
                if mode == 'offline':
                    system['membership']['peers'][0]['ready'] = False
                    system['membership']['peers'][0]['boot_id'] = None
                page.evaluate('amityInterface.refresh()')
                expect(rows).to_have_count(2)
                before = len([p for p, _ in fixture['posts'] if p == '/api/membership/remove'])
                rows.nth(1).get_by_role('button', name='Remove', exact=True).click()
                dialog = page.locator('dialog:visible')
                expect(dialog.get_by_role('button', name='Remove Amity', exact=True)).to_be_enabled()
                if mode == 'stale_revision':
                    system['membership']['membership_revision'] = '2'
                elif mode == 'stale_boot':
                    system['membership']['peers'][0]['boot_id'] = 'c'*32
                elif mode == 'stale_configuration':
                    fixture['config']['config_revision'] = str(int(saved_remove_config['config_revision'])+1)
                    fixture['config']['activities'].append(dict(activity_id='e'*32, label='New unreviewed activity', source=f'{31:032x}'))
                if mode.startswith('stale_'):
                    page.evaluate('amityInterface.refresh()')
                with page.expect_response(lambda r: r.url.endswith('/api/membership/remove')):
                    dialog.get_by_role('button', name='Remove Amity', exact=True).click()
                result = dialog.get_by_role('status')
                if mode.startswith('stale_'):
                    expect(result).to_contain_text('The Amity changed. Reopen Remove')
                    expect(dialog.get_by_role('button', name='Remove Amity', exact=True)).to_be_hidden()
                    assert system['membership']['peers']
                    dialog.get_by_role('button', name='Close', exact=True).click()
                    passed(mode+' retains reviewed confirmation tokens and rejects without drop or replay')
                elif mode == 'unsupported':
                    expect(result).to_contain_text('Update this Amity before removing it')
                    expect(dialog.get_by_role('button', name='Remove Amity', exact=True)).to_be_hidden()
                    assert system['membership']['peers'] == saved_peers
                    dialog.get_by_role('button', name='Close', exact=True).click()
                    passed('unsupported responding firmware rejects before removal/reset and never retries itself')
                else:
                    if mode not in ('post_error', 'poll_error'):
                        expect(dialog.get_by_role('button', name='Remove Amity', exact=True)).to_be_disabled()
                        expect(result).to_contain_text('Removing amity-bedroom')
                        assert system['membership']['peers']
                        expect(result).not_to_contain_text('Factory reset accepted')
                        fixture['remove_release'] = True
                    if mode == 'scheduled':
                        expect(result).to_contain_text('Factory reset accepted; it will restart and forget its Wi-Fi and settings.')
                        expect(result).not_to_contain_text('Factory reset completed')
                        passed('responsive removal waits for configuration cleanup and acknowledged reset scheduling')
                    elif mode in ('offline', 'unconfirmed'):
                        expect(result).to_contain_text('amity-bedroom removed.')
                        expect(result).to_contain_text('at least 10 seconds')
                        expect(result).not_to_contain_text('Factory reset accepted')
                        passed(mode+' removes saved data but requires physical reset without a reset-success claim')
                    else:
                        expect(result).to_contain_text('Removal could not be confirmed')
                        expect(result).to_contain_text('physical button before adding it again')
                        expect(result).not_to_contain_text('Factory reset accepted')
                        passed(mode+' remains unknown without destructive replay')
                    expect(dialog).to_be_visible()
                    expect(dialog.get_by_role('button', name='Remove Amity', exact=True)).to_be_hidden()
                    dialog.get_by_role('button', name='Close', exact=True).click()
                    assert [row['octopus_id'] for row in fixture['config']['octopuses']] == [controller]
                    assert fixture['config']['components'] == []
                    assert fixture['config']['activities'] == [dict(activity_id='f'*32, label='Unrelated local activity')]
                page.evaluate('amityInterface.refresh()')
                assert len([p for p, _ in fixture['posts'] if p == '/api/membership/remove']) == before + 1
                assert fixture['scans'] == before_scans
            assert all(path == '/api/membership/remove' for path, _ in fixture['posts'][before_remove:])
            fixture['config'] = deepcopy(saved_config)
            editor.clear()
            editor.update(saved_editor)
            system['membership']['peers'] = deepcopy(saved_peers)
            system['membership']['membership_revision'] = '1'
            page.evaluate('amityInterface.refresh()')
            expect(rows.first.locator('h3')).to_have_text('amity-living-room (this device)')
            expect(rows.first.get_by_role('button', name='Remove', exact=True)).to_have_count(0)
            # Successful device changes reload the saved activity projection.
            # A concurrent server deletion cannot silently erase a local draft.
            projection_baseline = deepcopy(editor)
            editor['activities'] = deepcopy(projection_baseline['activities'][:1])
            page.evaluate('refreshActivityConfiguration()')
            page.locator('[data-tab=activities]').click()
            expect(page.locator('#activityList article')).to_have_count(1)
            passed('completed device changes can immediately reload the saved activity projection')
            page.locator('#activityList article').first.get_by_role('button', name='Edit', exact=True).click()
            page.locator('#activityFields input').first.fill('Unrelated unsaved draft')
            page.locator('#activityForm button[type=submit]').click()
            editor['activities'] = []
            page.evaluate('refreshActivityConfiguration()')
            expect(page.locator('#message')).to_have_text('Activities changed. Reload the page before saving your edits.')
            expect(page.locator('#activityList')).to_contain_text('Unrelated unsaved draft')
            before_save = len([path for path, _ in fixture['posts'] if path == '/api/activity-editor'])
            page.locator('#saveActivities').click()
            expect(page.locator('#message')).to_have_text('Activities changed in another session. Reload the page before continuing.')
            assert len([path for path, _ in fixture['posts'] if path == '/api/activity-editor']) == before_save
            passed('device changes preserve unsaved activity drafts and require the existing reload conflict path')
            editor.clear()
            editor.update(projection_baseline)
            page.reload()
            expect(page.locator('#admin')).to_be_visible()
            page.locator('[data-tab=advanced]').click()
            backup = deepcopy(configuration)
            backup['controller_octopus_id'] = 'f'*32
            upload = dict(name='amity-config.json', mimeType='application/json', buffer=json.dumps(backup).encode())
            page.once('dialog', lambda dialog: dialog.dismiss())
            page.locator('#restore').set_input_files(upload)
            expect(page.locator('#restore')).to_have_value('')
            assert not [p for p, _ in fixture['posts'] if p.endswith('/restore')]
            confirmations = []
            def confirm_restore(dialog):
                confirmations.append(dialog.message)
                dialog.accept()
            page.once('dialog', confirm_restore)
            page.locator('#restore').set_input_files(upload)
            expect(page.locator('#message')).to_have_text('Configuration restored. Re-pair the remote and re-enroll added Amity devices before repairing activities.')
            assert 'powered off and disconnected' in confirmations[0]
            restored = [b for p, b in fixture['posts'] if p.endswith('/restore')]
            assert len(restored) == 1 and restored[0]['configuration'] == backup
            assert not [p for p, _ in fixture['posts'] if p in ('/api/restore', '/api/config')]
            passed('existing Advanced restore confirms old-main isolation and submits guarded portable recovery without playback')

            # Fresh pages cover both compiled adapter capacities on a lone
            # main, including initial hydration, scan and editor choices.
            saved_editor, saved_status = deepcopy(editor), deepcopy(status)
            saved_boards = deepcopy(fixture['config']['octopuses'])
            saved_system = deepcopy(system)
            lone = None
            try:
                system['named_devices'] = [dict(octopus_id=controller, hostname='amity-living-room')]
                system['membership']['peers'] = []
                for capacity, location in ((1, ''), (4, 'Adapter 1')):
                    fixture['config']['octopuses'] = [dict(b, supported_tentacles=capacity)
                                                      for b in saved_boards if b['octopus_id'] == controller]
                    local_choices = [dict(c, location=location) for c in choices[:2]]
                    editor['device_choices'] = local_choices
                    editor['activities'] = [dict(activity_id=aid, name='Watch TV', display=local_choices[0]['value'],
                                                 source=local_choices[1]['value'], audio=local_choices[0]['value'])]
                    status['device_choices'] = local_choices
                    status['devices'] = [dict(device, location=location) for device in saved_status['devices'][:2]]
                    status['current_activity'] = -1
                    lone = browser.new_page(viewport=dict(width=430, height=900))
                    lone.on('pageerror', lambda error: errors.append(str(error)))
                    lone.goto(f'http://127.0.0.1:{server.server_port}/')
                    lone.locator('#password').fill('fixture')
                    lone.locator('#password').press('Enter')
                    expect(lone.locator('#admin')).to_be_visible()
                    lone.locator('#discover').click()
                    expect(lone.locator('#deviceList .device')).to_have_count(2)
                    if capacity == 1:
                        expect(lone.locator('#deviceList .device-location')).to_have_count(0)
                        expect(lone.locator('#deviceList')).not_to_contain_text('Adapter')
                        screenshot = 'controller-lone-main-locations-ui-0.1.73.png'
                    else:
                        expect(lone.locator('#deviceList .device-location')).to_have_text(['Adapter 1', 'Adapter 1'])
                        screenshot = 'controller-lone-main-multi-adapter-locations-ui-0.1.73.png'
                    expect(lone.locator('#deviceList')).not_to_contain_text('amity-living-room')
                    assert not re.search(r'HDMI adapter|screen connection|source connection', lone.locator('#deviceList').inner_text(), re.I)
                    lone.screenshot(path=str(ROOT/'test-results'/screenshot), full_page=True)
                    lone.locator('#activityList article').first.get_by_role('button', name='Edit', exact=True).click()
                    suffix = f' · {location}' if location else ''
                    expect(lone.locator('#activityFields input').nth(1)).to_have_value('Display' + suffix)
                    expect(lone.locator('#activityFields input').nth(2)).to_have_value('Apple TV' + suffix)
                    expect(lone.locator('#sourceDeviceNames option')).to_have_attribute('value', 'Apple TV' + suffix)
                    lone.locator('#cancelActivity').click()
                    passed('a lone main with one adapter uses plain equipment names without location labels' if capacity == 1
                           else 'a lone main with multiple adapters keeps Adapter 1 in equipment rows, editor fields and device choices')
                    lone.close()
                    lone = None
            finally:
                if lone is not None:
                    lone.close()
                editor.clear()
                editor.update(saved_editor)
                status.clear()
                status.update(saved_status)
                fixture['config']['octopuses'] = saved_boards
                system.clear()
                system.update(saved_system)
            saved_configuration, saved_management_system = deepcopy(fixture['config']), deepcopy(system)
            try:
                retained, replacement, component = (f'{n:032x}' for n in (91, 92, 93))
                old_name = dict(consortium_id='c'*32, octopus_id=retained, tentacle_id=0, side='front', osd_name='Stored TV')
                replacement_name = {**old_name, 'octopus_id': replacement}
                fixture['config']['octopuses'] = [dict(octopus_id=controller, supported_tentacles=1),
                                                  dict(octopus_id=retained, supported_tentacles=1),
                                                  dict(octopus_id=replacement, supported_tentacles=1)]
                fixture['config']['components'] = [dict(component_id=component, kind='display', label='Stored TV',
                                                       qualified_names=[old_name])]
                system['named_devices'] = [dict(octopus_id=controller, hostname='amity-living-room'),
                                           dict(octopus_id=retained, hostname='amity-retained'),
                                           dict(octopus_id=replacement, hostname='amity-replacement')]
                system['membership']['peers'] = [dict(octopus_id=replacement, hostname='amity-replacement',
                                                       enrolled=True, ready=True, supported_tentacles=1,
                                                       configured_tentacles_mask=1, boot_id='d'*32)]
                system['observations'] = [dict(resolved=True, title='Replacement TV', qualified_name=replacement_name)]
                page.locator('[data-tab=amityDevices]').click()
                page.evaluate('() => window.amityInterface.refresh()')
                retained_row = page.locator('#managedAmityList article').filter(
                    has=page.get_by_role('heading', name='amity-retained', exact=True))
                expect(retained_row).to_have_count(1)
                retained_row.get_by_role('button', name='Replace', exact=True).click()
                replacement_dialog = page.locator('dialog:visible')
                expect(replacement_dialog).not_to_contain_text('The remote stays on the original Amity.')
                replacement_dialog.get_by_label('Replacement Amity', exact=True).select_option(replacement)
                expect(replacement_dialog.get_by_label('Replacement connection for Stored TV', exact=True)).to_have_count(1)
                expect(replacement_dialog.get_by_label('Replacement connection for Stored TV', exact=True).locator('option')).to_have_count(2)
                expect(replacement_dialog.get_by_label('Previous adapter 1 · Display', exact=True)).to_have_count(1)
                replacement_dialog.get_by_role('button', name='Cancel', exact=True).click()
                passed('management loads canonical saved owners and components while the activity editor keeps its projection')
            finally:
                fixture['config'].clear()
                fixture['config'].update(saved_configuration)
                system.clear()
                system.update(saved_management_system)
            assert not errors, errors
            passed('no browser script errors')
            browser.close()
    finally:
        initial_status_ready.set()
        server.shutdown()
    (ROOT/'test-results/controller-system-ui-0.1.73.json').write_text(json.dumps(dict(checks=checks, script_errors=errors), indent=2)+'\n')


if __name__ == '__main__':
    main()
