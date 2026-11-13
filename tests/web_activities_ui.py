#!/usr/bin/env python3
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

"""Exercise activity editing races and discovery visibility with a local API fixture."""
import json
from copy import deepcopy
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Event, Thread
from playwright.sync_api import sync_playwright, expect

ROOT = Path(__file__).resolve().parents[1]


def main():
    fixture = {
        'remote_unpair_gate': None, 'remote_unpair_async': False,
        'config': {'activities': [{'name': 'TV', 'display': 'TV', 'source': 'Player'},
                                               {'name': 'Music', 'display': '', 'source': 'Player', 'audio': 'Receiver'}]},
        'config_calls': 0, 'remotes': {'devices': [], 'bonded': False}, 'matter_remove_calls': [], 'fail_matter_remove': False, 'activity_calls': [], 'remote_calls': [], 'remote_pair_gate': None, 'fail_unpair': False, 'setup_error': False, 'configured': True, 'matter': {'fabrics': [], 'commissioning': False}, 'discover_gate': None, 'fail_discover': False, 'fail_save': False, 'save_gate': None, 'expired': False, 'logins': 0,
        'status': {'version': 'test', 'hardware': 'Amity Board / Fin', 'free_heap': 200000,
                   'wifi_connected': True, 'wifi_rssi': -45, 'wifi_channel': 6,
                   'ready': True, 'discovering': False, 'discovery_generation': 1,
                   'current_activity': -1, 'front_address': 4, 'back_address': 0,
                   'tx_errors': 10, 'rx_dropped': 5, 'devices': [
                       {'name': 'TV', 'bus': 'front', 'type': 0, 'address': 0, 'physical_address': 0},
                       {'name': 'Receiver', 'bus': 'back', 'type': 5, 'address': 5, 'physical_address': 4096},
                       {'name': 'Player', 'bus': 'back', 'type': 4, 'address': 4, 'physical_address': 8192}]}}

    # Human-facing fixture state is projected through the same current editor
    # DTO as shipping Amity; the backup endpoint serves only current saved data.
    group, controller, zone, boot = (f'{value:032x}' for value in (1, 2, 3, 4))
    fixture.update(config_revision=1, presentation_revision=1, state_revision=1, next_activity_id=100)
    equipment = {}
    saved_equipment = set()

    def component(name, role=None):
        if not name:
            return ''
        if role:
            saved_equipment.add(name)
        if name not in equipment:
            observed = next((d for d in fixture['status']['devices'] if d['name'] == name), {})
            kind = observed.get('type', {'display': 0, 'audio': 5, 'switch': 6}.get(role, 4))
            roles = ['display', 'audio'] if kind == 0 else ['audio', 'switch'] if kind == 5 else ['switch'] if kind in (6, 7) else ['source'] if kind in (1, 3, 4) else []
            equipment[name] = dict(value='component:' + f'{len(equipment) + 20:032x}', title=name, location='', roles=roles)
        if role and role not in equipment[name]['roles']:
            equipment[name]['roles'].append(role)
        return equipment[name]['value']

    def normalize_rows():
        for row in fixture['config']['activities']:
            if 'activity_id' not in row:
                row['activity_id'] = f'{fixture["next_activity_id"]:032x}'
                fixture['next_activity_id'] += 1
            for role in ('display', 'source', 'audio'):
                row.setdefault(role, '')
                component(row[role], role)
            if row.get('switch'):
                component(row['switch']['device'], 'switch')

    def choices():
        normalize_rows()
        for device in fixture['status']['devices']:
            component(device['name'])
        observed = {d['name'] for d in fixture['status']['devices']}
        return deepcopy([choice for name, choice in equipment.items() if choice['roles'] and (name in saved_equipment or name in observed)])

    def editor():
        available = choices()
        rows = deepcopy(fixture['config']['activities'])
        for row in rows:
            for role in ('display', 'source', 'audio'):
                row[role] = component(row[role], role)
            if row.get('switch'):
                row['switch']['device'] = component(row['switch']['device'], 'switch')
        return dict(version=1, expected_config_revision=str(fixture['config_revision']),
                    expected_presentation_revision=str(fixture['presentation_revision']),
                    activities=rows, device_choices=available)

    def editor_status():
        result = deepcopy(fixture['status'])
        result.update(expected_config_revision=str(fixture['config_revision']),
                      expected_presentation_revision=str(fixture['presentation_revision']),
                      expected_state_revision=str(fixture['state_revision']), expected_boot_id=boot,
                      device_choices=choices(), activity_settling=False)
        for device in result['devices']:
            device.update(title=device['name'], location='', name=component(device['name']))
        return result

    def configuration():
        dto = editor()
        rows = []
        for row in dto['activities']:
            value = dict(activity_id=row['activity_id'], zone_id=zone, label=row['name'], generated_label=row['name'], label_override=True,
                         **{role: row[role].removeprefix('component:') or None for role in ('display', 'source', 'audio')},
                         switch_device=row.get('switch', {}).get('device', '').removeprefix('component:') or None,
                         switch_input=row.get('switch', {}).get('input', 0))
            rows.append(value)
        order = [row['activity_id'] for row in rows]
        return dict(version=3, consortium_id=group, controller_octopus_id=controller, primary_zone_id=zone,
                    config_revision=str(fixture['config_revision']), presentation_revision=str(fixture['presentation_revision']),
                    octopuses=[dict(octopus_id=controller, label='amity-test', generated_label='amity-test', label_override=False, supported_tentacles=1)],
                    components=[dict(component_id=c['value'].removeprefix('component:'), label=c['title'], generated_label=c['title'], label_override=False,
                                     kind='display' if 'display' in c['roles'] else 'receiver' if 'audio' in c['roles'] else 'switch' if 'switch' in c['roles'] else 'source',
                                     qualified_names=[dict(consortium_id=group, octopus_id=controller, tentacle_id=0,
                                                           side='front' if 'display' in c['roles'] else 'back', osd_name=c['title'][:14])])
                                for c in dto['device_choices']],
                    activities=rows, activity_order=order,
                    zones=[dict(zone_id=zone, label='Main screen', generated_label='Main screen', label_override=False, activity_order=order)],
                    external_slots=[dict(slot=index, activity_id=value, tombstone=False, hidden=False, alias='') for index, value in enumerate(order)])

    def visible_name(value):
        if not value:
            return ''
        matches = [name for name, choice in equipment.items() if choice['value'] == value]
        assert len(matches) == 1, 'Editor saves must retain exact current equipment selectors'
        return matches[0]

    # An unused retained selection is available to the current editor even
    # while absent from scan results and activity-derived suggestions.
    component('New Console', 'source')

    class Handler(SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=str(ROOT / 'components/web_admin/assets'), **kwargs)

        def log_message(self, *args):
            pass

        def end_headers(self):
            self.send_header('Content-Security-Policy', "default-src 'self'; script-src 'self'; style-src 'self'")
            super().end_headers()

        def reply(self, body, status=200):
            data = json.dumps(body).encode()
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if self.path == '/api/setup':
                if fixture['setup_error']:
                    return self.reply({'error': 'Fixture startup failed'}, 500)
                return self.reply({'required': False})
            if self.path.startswith('/api/') and fixture['expired']:
                return self.reply({'error': 'Login required'}, 401)
            endpoints = {'/api/config': configuration(), '/api/activity-editor': editor(), '/api/activity-editor/status': editor_status(),
                         '/api/system': dict(mode='coordinator', octopus_id=controller, consortium_id=group, boot_id=boot,
                                             hostname='amity-test', named_devices=[dict(octopus_id=controller, hostname='amity-test')],
                                             membership=dict(coordinator=True, authority_ready=True, member=False, membership_revision='1', peers=[])),
                         '/api/homekit': dict(available=False, paired=False),
                         '/api/remotes': fixture['remotes'],
                         '/api/matter': fixture['matter']}
            if self.path in endpoints:
                return self.reply(deepcopy(endpoints[self.path]))
            super().do_GET()

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers.get('Content-Length', '0'))))
            if self.path == '/api/login':
                fixture['logins'] += 1
                fixture['expired'] = False
                return self.reply({'token': 'fixture-session'})
            if self.path == '/api/activity-editor/activity':
                assert body['expected_config_revision'] == str(fixture['config_revision'])
                assert body['expected_state_revision'] == str(fixture['state_revision']) and body['expected_boot_id'] == boot
                assert 'index' not in body
                index = next((i for i, row in enumerate(fixture['config']['activities']) if row['activity_id'] == body['activity_id']), -1)
                assert body['activity_id'] is None or index >= 0
                fixture['activity_calls'].append(index)
                fixture['status']['current_activity'] = index
                fixture['state_revision'] += 1
                return self.reply(dict(operation_id=body['request_id'], stage='complete'))
            if self.path == '/api/activity-editor':
                assert body['version'] == 1
                assert body['expected_config_revision'] == str(fixture['config_revision'])
                assert body['expected_presentation_revision'] == str(fixture['presentation_revision'])
                fixture['config_calls'] += 1
                if fixture['fail_save']:
                    return self.reply({'error': 'Could not save configuration'}, 500)
                rows = deepcopy(body['activities'])
                for row in rows:
                    for role in ('display', 'source', 'audio'):
                        row[role] = visible_name(row[role])
                    if row.get('switch'):
                        row['switch']['device'] = visible_name(row['switch']['device'])
                active_index = fixture['status']['current_activity']
                active = fixture['config']['activities'][active_index] if active_index >= 0 else None
                if active and next((row for row in rows if row.get('activity_id') == active['activity_id']), None) != active:
                    return self.reply({'error': 'Stop this activity before editing or deleting it.'}, 409)
                fixture['config'] = dict(activities=rows)
                normalize_rows()
                fixture['config_revision'] += 1
                fixture['status']['current_activity'] = next((index for index, row in enumerate(rows)
                    if active and row['activity_id'] == active['activity_id']), -1)
                fixture['state_revision'] += 1
                if fixture['save_gate']:
                    assert fixture['save_gate'].wait(20)
                return self.reply(dict(operation_id=body['request_id'], stage='applied', saved=True))
            if self.path.startswith('/api/remotes/'):
                fixture['remote_calls'].append(self.path)
            if self.path == '/api/remotes/scan':
                fixture['remotes'] = {'devices': [], 'bonded': False, 'scanning': True}
            if self.path == '/api/remotes/pair':
                assert body == {}, 'UI must request automatic pairing without selecting an address'
                if fixture['remote_pair_gate']:
                    assert fixture['remote_pair_gate'].wait(20)
                fixture['remotes'] = {'devices': [], 'bonded': False, 'pairing': True, 'scanning': True}
            if self.path == '/api/remotes/stop':
                assert body == {}
                fixture['remotes'] = {'devices': [], 'bonded': False}
            if self.path == '/api/remotes/unpair':
                if fixture['remote_unpair_gate']:
                    assert fixture['remote_unpair_gate'].wait(20)
                if fixture['fail_unpair']:
                    return self.reply({'error': 'Could not unpair'}, 500)
                if fixture['remote_unpair_async']:
                    fixture['remotes']['unpairing'] = True
                else:
                    fixture['remotes'] = {'devices': [], 'bonded': False}
            if self.path == '/api/matter/remove':
                if fixture['fail_matter_remove']:
                    return self.reply({'error': 'Could not remove connection'}, 500)
                fixture['matter_remove_calls'].append(body['index'])
                removed_home = any(f['index'] == body['index'] and f['vendor'] == 0x1349
                                   for f in fixture['matter']['fabrics'])
                fixture['matter']['fabrics'] = [f for f in fixture['matter']['fabrics'] if f['index'] != body['index']]
                if removed_home and not any(f['vendor'] == 0x1349 for f in fixture['matter']['fabrics']):
                    fixture['matter']['fabrics'] = [f for f in fixture['matter']['fabrics'] if f['vendor'] != 0x1384]
            if self.path == '/api/matter/open':
                fixture['matter'] = {'fabrics': [], 'commissioning': True, 'manual_code': '12345678901', 'qr_modules': ['1' * 21] * 21}
            if self.path == '/api/matter/close':
                fixture['matter'] = {'fabrics': [], 'commissioning': False}
            if self.path == '/api/activity-editor/discover':
                if fixture['fail_discover']:
                    return self.reply({'error': 'Could not scan HDMI'}, 500)
                if fixture['discover_gate']:
                    assert fixture['discover_gate'].wait(20)
                fixture['status']['discovering'] = True
                return self.reply(dict(operation_id=body['request_id'], stage='complete'))
            self.reply({'ok': True})

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    Thread(target=server.serve_forever, daemon=True).start()
    checks = []

    def passed(name):
        checks.append(name)
        print('PASS', name, flush=True)

    try:
        with sync_playwright() as p:
            browser = p.chromium.launch(headless=True)
            try:
                page = browser.new_page()
                page.set_default_timeout(10000)
                errors = []
                page.on('pageerror', lambda error: errors.append(str(error)))
                page.goto(f'http://127.0.0.1:{server.server_port}/')
                expect(page.locator('#login')).to_be_visible()
                expect(page.locator('#connection')).to_have_text('')
                page.locator('#password').fill('fixture-password')
                page.locator('#password').press('Enter')
                expect(page.locator('#admin')).to_be_visible()
                page.wait_for_function('() => !polling && !activityActionPending')
                assert fixture['logins'] == 1
                passed('Enter submits login once')
                expect(page.locator('#connection')).to_have_text('')
                fixture['status']['ready'] = False
                page.evaluate('() => refresh()')
                expect(page.locator('#connection')).to_have_text('')
                fixture['status']['ready'] = True
                passed('header is blank on login and while idle or not yet ready')
                fixture['status']['activity_error'] = 'Some equipment is not responding. Scan HDMI and try again.'
                page.wait_for_function('() => !polling')
                page.evaluate('() => refresh()')
                expect(page.locator('#message')).to_have_text(fixture['status']['activity_error'])
                passed('asynchronous activity failure is visible after admission')
                fixture['status']['activity_error'] = ''
                page.evaluate("() => message('')")
                save = page.locator('#saveActivities')
                discard = page.locator('#discardActivities');expect(discard).to_be_hidden()
                expect(save).to_be_hidden()
                expect(page.locator('[data-tab=devices]')).to_have_count(0)
                expect(page.locator('#devices')).to_be_hidden()
                passed('saved activities and cached devices need no extra controls')
                starts = page.locator('[data-start]')
                expect(page.locator('#standby')).to_be_hidden()
                expect(starts.first).to_have_text('Start')
                starts.first.click()
                expect(starts.first).to_have_text('Repair')
                expect(starts.nth(1)).to_have_text('Start')
                expect(page.locator('#standby')).to_be_visible()
                starts.first.click()
                page.wait_for_function('() => !polling && !activityActionPending')
                page.wait_for_function('() => !activityActionPending');assert fixture['activity_calls'] == [0, 0]
                passed('active activity offers Repair and resubmits the same activity')
                fixture['status']['current_activity'] = 1
                page.evaluate('() => refresh()')
                expect(starts.first).to_have_text('Start')
                expect(starts.nth(1)).to_have_text('Repair')
                page.locator('#standby').click()
                expect(page.locator('#standby')).to_be_hidden()
                expect(starts.nth(1)).to_have_text('Start')
                page.wait_for_function('() => !activityActionPending');assert fixture['activity_calls'] == [0, 0, -1]
                passed('external activity changes update Repair and Standby returns to inactive controls')
                def edit(index=0):
                    page.locator('#activityList article').nth(index).get_by_role('button',name='Edit',exact=True).click()
                    expect(page.locator('#activityEditor')).to_be_visible()
                    expect(page.locator('#activityForm button[type=submit]')).to_have_text('OK')
                def rename(value,index=0):
                    edit(index);page.locator('#activityFields input').first.fill(value)
                    page.locator('#activityForm').get_by_role('button',name='OK',exact=True).click()
                    expect(page.locator('#activityEditor')).to_be_hidden()
                def drag(source,target):
                    page.locator('#activityList').scroll_into_view_if_needed()
                    expected=page.locator('#activityList .activity-name').all_text_contents()
                    expected.insert(target,expected.pop(source))
                    handle=page.locator('[data-drag]').nth(source).bounding_box()
                    row=page.locator('#activityList article').nth(target).bounding_box()
                    page.mouse.move(handle['x']+handle['width']/2,handle['y']+handle['height']/2)
                    page.mouse.down();page.mouse.move(handle['x']+handle['width']/2,row['y']+row['height']/2,steps=10);page.mouse.up()
                    expect(page.locator('#activityList .activity-name')).to_have_text(expected)
                def standby():
                    page.locator('#standby').click()
                    expect(page.locator('#standby')).to_be_hidden()
                    page.wait_for_function('() => !polling && !activityActionPending')

                starts.first.click();page.wait_for_function('() => !activityActionPending')
                current_id=fixture['config']['activities'][0]['activity_id']
                current_row=deepcopy(fixture['config']['activities'][0])
                active_row=page.locator('#activityList article').first
                for control in ('Edit','Delete'):
                    expect(active_row.get_by_role('button',name=control,exact=True)).to_be_disabled()
                    expect(active_row.get_by_role('button',name=control,exact=True)).to_have_attribute('title','Stop this activity before editing or deleting it.')
                expect(page.locator('#activityList article').nth(1).get_by_role('button',name='Edit',exact=True)).to_be_enabled()
                page.evaluate('() => openActivityEditor(config.activities[0])')
                expect(page.locator('#activityEditor')).to_be_hidden()
                expect(page.locator('#message')).to_have_text('Stop this activity before editing or deleting it.')
                before_calls=len(fixture['activity_calls'])
                rename('Music during playback',1);save.click();expect(save).to_be_hidden()
                assert fixture['status']['current_activity']==0 and fixture['config']['activities'][0]==current_row
                assert len(fixture['activity_calls'])==before_calls
                rename('Music',1);save.click();expect(save).to_be_hidden()
                drag(0,1);save.click();expect(save).to_be_hidden()
                assert fixture['status']['current_activity']==1 and fixture['config']['activities'][1]['activity_id']==current_id
                expect(starts.nth(1)).to_have_text('Repair')
                expect(page.locator('#activityList article').nth(1).get_by_role('button',name='Delete',exact=True)).to_be_disabled()
                drag(1,0);save.click();expect(save).to_be_hidden();standby()
                passed('current activity cannot be edited or deleted; editing another activity and reordering preserve its identity and playback')

                edit(1);page.locator('#activityFields input').first.fill('Pending Music edit')
                page.evaluate('''() => {
                    window.savedAddComponent=window.amityInterface.addComponent;
                    window.amityInterface.addComponent=async (role,name,selected)=>{window.pendingEquipmentChoice=selected;};
                }''')
                page.locator('#activityFields input[data-device-role=source]').click()
                page.locator('#activityFields .device-options:not([hidden])').get_by_role('option',name='Add component…',exact=True).click()
                page.evaluate('() => { window.amityInterface.addComponent=window.savedAddComponent; }')
                page.locator('#activityFields input[data-device-role=source]').click()
                expect(page.locator('#activityFields .device-options:not([hidden])')).to_be_visible()
                fixture['status']['current_activity']=1;page.evaluate('() => refresh()')
                expect(page.locator('#activityFields input').first).to_be_disabled()
                expect(page.locator('#activityFields .device-options:not([hidden])')).to_have_count(0)
                assert page.locator('#activityFields button:enabled').count()==0
                expect(page.locator('#activityForm button[type=submit]')).to_be_disabled()
                expect(page.locator('#editorStart')).to_be_disabled()
                expect(page.locator('#activityEditorError')).to_have_text('Stop this activity before editing or deleting it.')
                page.evaluate("() => window.pendingEquipmentChoice('Changed source')")
                expect(page.locator('#activityFields input[data-device-role=source]')).to_have_value('Player')
                assert page.evaluate('() => editingActivity.activity.source')=='Player'
                page.locator('#cancelActivity').click();expect(save).to_be_hidden();standby()
                rename('Staged TV edit');fixture['status']['current_activity']=0;page.evaluate('() => refresh()')
                expect(save).to_be_disabled();writes_before=fixture['config_calls']
                page.evaluate('() => persistActivities().catch(reportError)')
                expect(page.locator('#message')).to_have_text('Stop this activity before editing or deleting it.')
                assert fixture['config_calls']==writes_before
                discard.click();standby()
                page.locator('#activityList article').nth(1).get_by_role('button',name='Delete',exact=True).click()
                fixture['status']['current_activity']=1;page.evaluate('() => refresh()');expect(save).to_be_disabled()
                discard.click();standby()
                passed('activity changes arriving during an open dialog or after staged edits or deletion block saving the now current activity')
                expect(page.locator('#activityList input')).to_have_count(0)
                expect(page.locator('#activityList article').first.locator('button')).to_have_text(['Edit','Delete','Start'])
                passed('main list contains only names, reorder handles, and Edit/Delete/Start controls')
                saved_before=deepcopy(fixture['config']);writes_before=fixture['config_calls'];activity_before=fixture['status']['current_activity']
                rename('Unwanted edit');page.locator('#activityList article').nth(1).get_by_role('button',name='Delete',exact=True).click()
                page.locator('#addActivity').click();page.locator('#activityForm').get_by_role('button',name='Add',exact=True).click();drag(0,1)
                expect(discard).to_be_visible();discard.click()
                expect(page.locator('#activityList .activity-name')).to_have_text(['TV','Music']);expect(save).to_be_hidden();expect(discard).to_be_hidden()
                assert fixture['config']==saved_before and fixture['config_calls']==writes_before and fixture['status']['current_activity']==activity_before
                assert page.evaluate('() => config.activities')==saved_before['activities']
                expect(starts.first).to_be_enabled();edit();expect(page.locator('#activityFields input').first).to_have_value('TV');page.locator('#cancelActivity').click()
                rename('Unwanted again');discard.click();expect(page.locator('#activityList .activity-name')).to_have_text(['TV','Music'])
                passed('Discard restores edits, deletions, additions and order without writing or changing the active activity')
                rename('Failed staged save');fixture['fail_save']=True;save.click();expect(page.locator('#message')).to_have_text('Could not save configuration')
                expect(discard).to_be_enabled();discard.click();expect(save).to_be_hidden();expect(discard).to_be_hidden();expect(page.locator('#message')).to_have_text('')
                assert fixture['config']==saved_before;fixture['fail_save']=False
                passed('Discard remains available after a failed save and clears its stale error')
                edit();page.locator('#editorStart').click();expect(page.locator('#editorStart')).to_have_text('Repair')
                page.locator('#editorStart').click();expect(page.locator('#editorStart')).to_be_enabled()
                page.wait_for_function('() => !activityActionPending');assert fixture['activity_calls'][-2:]==[0,0]
                expect(page.locator('#activityFields input').first).to_be_disabled()
                expect(page.locator('#activityForm button[type=submit]')).to_be_disabled()
                expect(page.locator('#activityList .activity-name').first).to_have_text('TV')
                page.locator('#cancelActivity').click();expect(save).to_be_hidden();standby()
                passed('modal starts and repairs saved activity; its fields become read only until standby')
                edit();page.locator('#activityFields input').first.fill('Escape edit');page.keyboard.press('Escape')
                expect(page.locator('#activityEditor')).to_be_hidden();expect(save).to_be_hidden()
                passed('Escape dismisses modal without applying edits')
                edit();page.locator('#activityFields input').first.fill('Backdrop edit')
                dialog=page.locator('#activityEditor');box=dialog.bounding_box();writes_before=fixture['config_calls']
                page.mouse.click(box['x']+5,box['y']+5);expect(dialog).to_be_visible()
                page.mouse.move(box['x']+5,box['y']+5);page.mouse.down();page.mouse.move(5,5,steps=5);page.mouse.up();expect(dialog).to_be_visible()
                page.mouse.click(5,5);expect(dialog).to_be_hidden();expect(save).to_be_hidden()
                assert fixture['config_calls']==writes_before and fixture['config']['activities'][0]['name']=='TV'
                expect(page.locator('#activityList .activity-name').first).to_have_text('TV')
                passed('activity backdrop discards edits without saving; inside padding and a press dragged outside do not cancel')
                rename('Changed');expect(save).to_be_visible();expect(starts.first).to_be_disabled();expect(starts.nth(1)).to_be_enabled()
                edit();page.locator('#activityFields input').first.fill('TV');before_calls=len(fixture['activity_calls'])
                expect(page.locator('#editorStart')).to_have_text('Start');page.locator('#editorStart').click();page.wait_for_function('() => !activityActionPending')
                assert len(fixture['activity_calls'])==before_calls+1 and fixture['activity_calls'][-1]==0
                assert fixture['config']['activities'][0]['name']=='TV'
                page.locator('#cancelActivity').click();expect(page.locator('#activityList .activity-name').first).to_have_text('Changed');standby()
                passed('modal draft reverted to saved settings can start without persisting other staged edits')
                rename('TV');expect(save).to_be_hidden();expect(starts.first).to_be_enabled()
                passed('reverting staged edits restores saved baseline; unaffected saved activity stays runnable')
                edit();page.locator('#activityFields input').nth(3).fill('Receiver');page.locator('#activityForm').get_by_role('button',name='OK',exact=True).click();expect(save).to_be_visible()
                edit();page.locator('#activityFields input').nth(3).fill('');page.locator('#activityForm').get_by_role('button',name='OK',exact=True).click();expect(save).to_be_hidden()
                passed('missing and empty optional fields compare equally')
                page.locator('#addActivity').click();expect(page.locator('#activityEditorTitle')).to_have_text('Add activity');expect(page.locator('#activityList article')).to_have_count(2)
                expect(page.locator('#activityForm button[type=submit]')).to_have_text('Add')
                page.locator('#activityFields input').first.fill('Discarded new activity');page.mouse.click(5,5)
                expect(page.locator('#activityEditor')).to_be_hidden();expect(page.locator('#activityList article')).to_have_count(2);expect(save).to_be_hidden()
                assert fixture['config_calls']==writes_before
                page.locator('#addActivity').click();page.locator('#activityForm').get_by_role('button',name='Add',exact=True).click();expect(save).to_be_visible()
                page.locator('#activityList article').last.get_by_role('button',name='Delete',exact=True).click();expect(save).to_be_hidden()
                passed('Add opens draft modal; cancel adds nothing; adding then deleting restores baseline')
                drag(0,1);expect(save).to_be_visible();expect(page.locator('#activityList .activity-name')).to_have_text(['Music','TV'])
                starts.first.click();expect(starts.first).to_have_text('Repair');page.wait_for_function('() => !activityActionPending');assert fixture['activity_calls'][-1]==1
                edit(1);page.locator('#editorStart').click();expect(page.locator('#editorStart')).to_have_text('Repair');page.wait_for_function('() => !activityActionPending');assert fixture['activity_calls'][-1]==0;page.locator('#cancelActivity').click()
                passed('dragged list and modal Start target saved indices, not unsaved positions')
                page.locator('[data-drag]').nth(1).press('ArrowUp');expect(save).to_be_hidden()
                expect(page.locator('#activityList .activity-name')).to_have_text(['TV','Music'])
                passed('keyboard reorder and reversing a drag restore the persisted order')
                standby();page.locator('#activityList article').first.get_by_role('button',name='Delete',exact=True).click();starts.first.click();expect(starts.first).to_have_text('Repair');page.wait_for_function('() => !activityActionPending');assert fixture['activity_calls'][-1]==1
                page.reload();expect(page.locator('#admin')).to_be_visible();page.wait_for_function('() => !polling && !activityActionPending')
                passed('deleting an earlier draft row never shifts the index sent to the device')
                fixture['fail_save']=True;edit();page.locator('#activityFields input').first.fill('Changed');before_calls=len(fixture['activity_calls']);page.locator('#editorStart').click()
                expect(page.locator('#activityEditorError')).to_have_text('Could not save configuration');expect(page.locator('#editorStart')).to_be_enabled()
                assert len(fixture['activity_calls'])==before_calls and fixture['config']['activities'][0]['name']=='TV'
                expect(page.locator('#activityList .activity-name').first).to_have_text('TV')
                passed('failed Save and start retains modal draft without changing the list or starting stale settings')
                fixture['fail_save']=False;page.locator('#editorStart').click();expect(page.locator('#editorStart')).to_have_text('Repair');expect(page.locator('#editorStart')).to_be_enabled()
                assert fixture['config']['activities'][0]['name']=='Changed' and fixture['activity_calls'][-1]==0
                page.locator('#cancelActivity').click();expect(save).to_be_hidden();standby()
                rename('TV');expect(save).to_be_visible();rename('Changed');expect(save).to_be_hidden()
                passed('Save and start persists first; successful save becomes comparison baseline')
                rename('Unwanted after save');discard.click();expect(page.locator('#activityList .activity-name')).to_have_text(['Changed','Music'])
                expect(starts.first).to_have_text('Start');expect(starts.first).to_be_enabled();expect(save).to_be_hidden();expect(discard).to_be_hidden()
                assert page.evaluate('() => config.activities')==fixture['config']['activities']
                passed('Discard uses the latest successful save and restores correct Repair mapping')
                rename('Submitted');gate=Event();fixture['save_gate']=gate;save.click();expect(save).to_be_disabled()
                assert page.locator('#activities button:enabled').count()==0
                expect(page.locator('[data-drag]').first).to_have_attribute('aria-disabled','true')
                page.locator('[data-drag]').first.press('ArrowDown');expect(page.locator('#activityList .activity-name').first).to_have_text('Submitted')
                gate.set();page.wait_for_function('() => !saving');expect(save).to_be_hidden();fixture['save_gate']=None
                passed('saving locks editing, starting and drag order until persisted indices are known')
                edit();page.locator('#activityFields input').first.fill('Music');page.locator('#activityForm').get_by_role('button',name='OK',exact=True).click()
                expect(page.locator('#activityEditorError')).to_have_text('Activity names must be unique.');page.locator('#cancelActivity').click();expect(save).to_be_hidden()
                passed('duplicate activity names stay in the modal for correction')
                baseline=deepcopy(fixture['config']);before_calls=len(fixture['activity_calls'])
                fixture['config']['activities'].reverse();starts.first.click()
                expect(page.locator('#message')).to_contain_text('Activities changed in another session')
                assert len(fixture['activity_calls'])==before_calls
                rename('Unsaved local');save.click();expect(page.locator('#message')).to_contain_text('Activities changed in another session')
                assert fixture['config']['activities'][0]['name']=='Music'
                fixture['config']=baseline;page.reload();expect(page.locator('#admin')).to_be_visible();page.wait_for_function('() => !polling && !activityActionPending')
                passed('changes saved by another session block stale starts and overwrites')
                edit();page.locator('#activityFields input').first.fill('Draft survives polling')
                fixture['status']['discovering']=True;page.evaluate('() => refresh()')
                assert page.locator('#activityEditor button:enabled').count()==0
                page.keyboard.press('Escape');expect(page.locator('#activityEditor')).to_be_visible()
                fixture['status']['discovering']=False;page.wait_for_function('() => !polling && !activityActionPending');page.evaluate('() => refresh()')
                expect(page.locator('#activityFields input').first).to_have_value('Draft survives polling')
                page.locator('#cancelActivity').click();expect(save).to_be_hidden()
                passed('external scans lock the open dialog and polling preserves its draft')
                drag(0,1);page.locator('#addActivity').click();page.locator('#activityFields input').first.fill('New from dialog');page.locator('#editorStart').click()
                expect(page.locator('#editorStart')).to_have_text('Repair');expect(page.locator('#editorStart')).to_be_enabled()
                page.wait_for_function('() => !activityActionPending');assert fixture['activity_calls'][-1]==2
                assert [a['name'] for a in fixture['config']['activities']]==['Music','Submitted','New from dialog']
                page.locator('#cancelActivity').click();expect(save).to_be_hidden();starts.nth(1).click();expect(starts.nth(1)).to_have_text('Repair');page.wait_for_function('() => !activityActionPending');assert fixture['activity_calls'][-1]==1
                passed('new modal activity saves pending reorder before starting its actual persisted index')
                fixture['config']=baseline;fixture['status']['current_activity']=-1;page.reload();expect(page.locator('#admin')).to_be_visible();page.wait_for_function('() => !polling && !activityActionPending')
                mobile=browser.new_context(viewport={'width':390,'height':844},is_mobile=True,has_touch=True)
                mobile.add_init_script("sessionStorage.setItem('amityToken','fixture-session')")
                touch=mobile.new_page();touch.goto(f'http://127.0.0.1:{server.server_port}/');expect(touch.locator('#admin')).to_be_visible()
                client=mobile.new_cdp_session(touch);handle=touch.locator('[data-drag]').first.bounding_box();target=touch.locator('#activityList article').nth(1).bounding_box()
                x=handle['x']+handle['width']/2;y=handle['y']+handle['height']/2;end=target['y']+target['height']/2
                client.send('Input.dispatchTouchEvent',{'type':'touchStart','touchPoints':[{'x':x,'y':y}]})
                for step in range(1,11):client.send('Input.dispatchTouchEvent',{'type':'touchMove','touchPoints':[{'x':x,'y':y+(end-y)*step/10}]})
                client.send('Input.dispatchTouchEvent',{'type':'touchEnd','touchPoints':[]})
                expect(touch.locator('#activityList .activity-name')).to_have_text(['Music','Submitted'])
                touch.locator('#activityList article').first.get_by_role('button',name='Edit',exact=True).click();expect(touch.locator('#activityEditor')).to_be_visible()
                assert touch.evaluate('document.documentElement.scrollWidth<=innerWidth')
                touch.screenshot(path=str(ROOT/'test-results/activity-modal-mobile.png'))
                touch.locator('#cancelActivity').click();mobile.close()
                edit();page.screenshot(path=str(ROOT/'test-results/activity-modal-desktop.png'));page.locator('#cancelActivity').click()
                passed('touch dragging and modal layout work on a narrow mobile viewport')
                fixture['status']['discovering'] = True
                page.evaluate('() => refresh()')
                assert page.locator('#activities button:enabled').count() == 0
                fixture['status']['discovering'] = False
                page.evaluate('() => refresh()')
                expect(page.locator('#addActivity')).to_be_enabled()
                passed('a scan started elsewhere disables all activity buttons')
                fixture['fail_discover'] = True
                page.locator('#discover').click()
                expect(page.locator('#message')).to_have_text('Could not scan HDMI')
                expect(page.locator('#discover')).to_be_enabled()
                expect(page.locator('#addActivity')).to_be_enabled()
                fixture['fail_discover'] = False
                passed('failed scan request restores activity buttons')
                scan_gate = Event()
                fixture['discover_gate'] = scan_gate
                page.locator('#discover').click()
                expect(page.locator('#addActivity')).to_be_disabled()
                assert page.locator('#activities button:enabled').count() == 0
                page.locator('[data-drag]').first.press('ArrowDown')
                expect(page.locator('#activityList .activity-name').first).to_have_text('Submitted')
                assert page.locator('#activities button:enabled').count() == 0
                scan_gate.set()
                fixture['discover_gate'] = None
                passed('all activity buttons stay disabled during scan submission and form edits')
                expect(page.locator('#devices')).to_be_visible()
                expect(page.locator('#deviceList .device')).to_have_count(0)
                expect(page.locator('#recommendations')).to_be_hidden()
                expect(page.locator('#connection')).to_have_text('Scanning HDMI…')
                assert page.locator('#activities button:enabled').count() == 0
                passed('manual scan reveals progress with every activity button disabled')
                fixture['status']['discovering'] = False
                fixture['status']['discovery_generation'] += 1
                page.wait_for_function('() => !polling && !activityActionPending')
                page.evaluate('() => refresh()')
                expect(page.locator('#deviceList .device')).to_have_count(3)
                expect(page.locator('#connection')).to_have_text('')
                assert page.locator('#deviceList .device').all_text_contents() == ['Player', 'Receiver', 'TV']
                assert page.locator('#deviceList svg').evaluate_all('(icons) => icons.map(icon => icon.getAttribute("aria-label"))') == ['Source', 'Audio', 'TV']
                passed('device rows show only sorted OSD names with role icons')
                expect(page.locator('#addActivity')).to_be_enabled()
                expect(save).to_be_hidden()
                assert all(b.is_enabled() for b in page.locator('[data-start]').all())
                passed('scan completion restores buttons while preserving unsaved-edit restrictions')
                fixture['status']['devices'].extend([
                    {'name': 'HDMI Switch', 'bus': 'back', 'type': 6, 'address': 14, 'physical_address': 8192},
                    {'name': 'Unknown device', 'bus': 'back', 'type': 255, 'address': 12, 'physical_address': 12288}])
                fixture['status']['discovery_generation'] += 1
                page.evaluate('() => refresh()')
                expect(page.locator('#deviceList svg[aria-label="HDMI switch"]')).to_have_count(1)
                expect(page.locator('#deviceList svg[aria-label="Device"]')).to_have_count(1)
                passed('switch and unknown devices have distinct fallback icons')
                fixture['status']['devices'] = fixture['status']['devices'][:3]
                fixture['status']['discovery_generation'] += 1
                page.evaluate('() => refresh()')
                expect(page.locator('#recommendations')).to_be_visible()
                expect(page.locator('#standby')).to_be_hidden()
                assert page.locator('#discover').bounding_box()['y'] < page.locator('#activityList').bounding_box()['y']
                passed('Scan HDMI is at the top and Standby is hidden while inactive')
                before = page.locator('#activityList article').count()
                page.locator('[data-recommendation]:visible').first.get_by_role('button', name='Edit', exact=True).click()
                expect(page.locator('#activityEditor')).to_be_visible()
                expect(page.locator('#activityEditorTitle')).to_have_text('Edit activity')
                expect(page.locator('#editorStart')).to_have_text('Save and start')
                assert page.locator('#activityFields input[data-device-list]').evaluate_all('(inputs) => inputs.map(input => input.getAttribute("data-device-list"))') == ['displayDeviceNames', 'sourceDeviceNames', 'audioDeviceNames', 'switchDeviceNames']
                passed('recommendations open in the activity editor with the same category-specific device lists')
                page.locator('#activityFields input').first.fill('My player')
                page.locator('#activityForm').get_by_role('button', name='OK', exact=True).click()
                expect(page.locator('[data-recommendation]:visible').first).to_contain_text('My player')
                expect(save).to_be_hidden()
                page.locator('[data-recommendation]:visible').first.get_by_role('button', name='Add', exact=True).click()
                expect(save).to_be_visible()
                expect(page.locator('#activityList article')).to_have_count(before + 1)
                expect(page.locator('#recommendations')).to_be_hidden()
                passed('recommendations are editable and added individually without replacing activities')
                added = page.locator('#activityList article').last
                edit(before);page.locator('#activityFields input').first.fill('Renamed activity')
                page.locator('#activityFields input').nth(4).fill('Receiver')
                page.locator('#activityForm').get_by_role('button',name='OK',exact=True).click()
                expect(page.locator('#recommendations')).to_be_hidden()
                passed('recommendation equivalence ignores name and manual switch settings')
                added.get_by_role('button', name='Delete', exact=True).click()
                expect(page.locator('#recommendations')).to_be_visible()
                expect(save).to_be_hidden()
                passed('removing an activity restores its recommendation')
                fixture['status']['devices'].append({'name': 'Nintendo Switch', 'bus': 'back', 'type': 4, 'address': 8, 'physical_address': 12288})
                page.locator('#discover').click()
                expect(page.locator('#discoveryStatus')).to_have_text('Scanning HDMI…')
                fixture['status']['discovering'] = False
                fixture['status']['discovery_generation'] += 1
                page.wait_for_function('() => !polling && !activityActionPending')
                page.evaluate('() => refresh()')
                expect(page.locator('#recommendationList')).to_contain_text('Play Nintendo Switch')
                passed('console recommendations use Play like hdmi_tool.py')
                page.evaluate('() => { dirty=false; }')
                fixture['config']['activities'].append({'name': 'Custom saved activity', 'display': 'TV', 'source': 'Player', 'audio': 'Receiver', 'switch': {'device': 'Manual switch', 'input': 7}})
                page.reload()
                expect(page.locator('#admin')).to_be_visible()
                page.locator('#discover').click()
                expect(page.locator('#discoveryStatus')).to_have_text('Scanning HDMI…')
                fixture['status']['discovering'] = False
                fixture['status']['discovery_generation'] += 1
                page.wait_for_function('() => !polling && !activityActionPending')
                page.evaluate('() => refresh()')
                expect(page.locator('[data-recommendation]')).to_have_count(1)
                expect(page.locator('#recommendationList')).to_contain_text('Play Nintendo Switch')
                assert 'Watch Player' not in page.locator('#recommendationList').text_content()
                passed('fresh scan excludes equivalent persisted activities despite different names and switches')
                fixture['status']['devices'] = [d for d in fixture['status']['devices'] if d['name'] != 'Nintendo Switch']
                fixture['status']['discovery_generation'] += 1
                page.evaluate('() => refresh()')
                expect(page.locator('[data-recommendation]')).to_have_count(0)
                expect(page.locator('#recommendations')).to_be_hidden()
                passed('no suggestions are rendered when all scanned activities are already configured')
                # Regression: TV audio must not produce a duplicate with an empty audio role.
                saved_config = deepcopy(fixture['config'])
                saved_devices = deepcopy(fixture['status']['devices'])
                fixture['config']['activities'] = [{'name': 'My TV player', 'display': 'TV', 'source': 'Player',
                                                    'audio': 'TV', 'switch': {'device': 'Manual switch', 'input': 2}}]
                fixture['status']['devices'] = [d for d in saved_devices if d['address'] != 5]
                page.reload()
                expect(page.locator('#admin')).to_be_visible()
                page.locator('#discover').click()
                expect(page.locator('#discoveryStatus')).to_have_text('Scanning HDMI…')
                fixture['status']['discovering'] = False
                fixture['status']['discovery_generation'] += 1
                page.wait_for_function('() => !polling && !activityActionPending')
                page.evaluate('() => refresh()')
                expect(page.locator('[data-recommendation]')).to_have_count(0)
                expect(page.locator('#recommendations')).to_be_hidden()
                passed('TV audio fallback excludes an equivalent saved activity without a receiver')
                # A different audio route is still a different activity under is_equivalent().
                fixture['status']['devices'] = saved_devices
                fixture['status']['discovery_generation'] += 1
                page.evaluate('() => refresh()')
                expect(page.locator('[data-recommendation]')).to_have_count(1)
                assert page.locator('.recommendation-name').evaluate('(el) => getComputedStyle(el).fontWeight') == '400'
                passed('different audio remains recommended and recommendation names are not bold')
                previous_activity, calls = fixture['status']['current_activity'], len(fixture['activity_calls'])
                page.locator('[data-recommendation]:visible').first.get_by_role('button', name='Edit', exact=True).click()
                page.locator('#activityFields input').first.fill('Suggested player')
                page.locator('#editorStart').click()
                expect(page.locator('#activityList article')).to_have_count(2)
                expect(page.locator('#activityList article').last.locator('.activity-badge')).to_be_visible()
                expect(page.locator('#editorStart')).to_have_text('Repair')
                expect(page.locator('#recommendations')).to_be_hidden()
                assert fixture['activity_calls'][calls:] == [1] and fixture['config']['activities'][1]['name'] == 'Suggested player'
                page.locator('#cancelActivity').click()
                fixture['status']['current_activity'] = previous_activity
                passed('Save and start on an edited recommendation adds, saves and starts it')
                fixture['config'] = saved_config
                fixture['status']['devices'].append({'name': 'Nintendo Switch', 'bus': 'back', 'type': 4, 'address': 8, 'physical_address': 12288})
                page.evaluate('() => { dirty=false; }')
                page.reload()
                expect(page.locator('#admin')).to_be_visible()
                expect(page.locator('#devices')).to_be_hidden()
                expect(save).to_be_hidden()
                passed('reload hides scan results and reloads saved activity baseline')
                page.locator('[data-tab=advanced]').click()
                expect(page.locator('#diagnostics')).to_contain_text('Firmware')
                diagnostics = page.locator('#diagnostics').text_content()
                assert not any(text in diagnostics for text in ['Amity Board', 'CEC front', 'Transmit failures', 'dropped frames', 'Free memory'])
                passed('settings omit hardcoded hardware and CEC counters')
                expect(page.locator('#advanced')).to_contain_text('Update Firmware')
                expect(page.locator('#advanced')).to_contain_text('Download Amity configuration for backup.')
                expect(page.locator('#backup')).to_have_text('Download')
                expect(page.locator('#advanced')).to_contain_text('Upload, and restore a previous configuration.')
                assert 'Upload an Amity firmware binary' not in page.locator('#advanced').text_content()
                for bonded in (False, True):
                    for extra, status in [({}, 'Disconnected'), ({'scanning': True}, 'Scanning'),
                                          ({'connecting': True}, 'Connecting'), ({'last_error': 8}, 'Connection failed')]:
                        fixture['remotes'] = {'devices': [], 'bonded': bonded, **extra}
                        page.evaluate('() => refresh()')
                        expected_remote_status = 'Paired' if bonded else 'Pairing with remote…' if extra.get('connecting') else 'Looking for a nearby remote… Bring it close and put it in pairing mode.' if extra.get('scanning') else 'Pairing failed. Reset the remote’s pairing and try again.' if extra.get('last_error') else 'Not paired'
                        expect(page.locator('#remoteStatus')).to_have_text(expected_remote_status)
                        expect(page.locator('#remotes progress, #remoteProgress')).to_have_count(0)
                        if bonded:
                            expect(page.locator('#toggleRemote')).to_have_text('Unpair remote')
                            expect(page.locator('#toggleRemote')).to_be_enabled()
                            expect(page.locator('#toggleRemote')).to_have_attribute('aria-busy', 'false')
                        diagnostic_status = 'Waiting for remote' if bonded and not extra else status
                        expect(page.locator('#diagnostics')).to_contain_text('Remote: ' + diagnostic_status)
                fixture['remotes'] = {'devices': [], 'bonded': True, 'connected': True, 'name': 'Atomic', 'battery': 85}
                page.evaluate('() => refresh()')
                expect(page.locator('#remoteStatus')).to_have_text('Paired')
                expect(page.locator('#diagnostics')).to_contain_text('Remote: Connected · Atomic · 85% battery')
                passed('paired Remote status stays Paired during scanning, reconnecting and errors, without progress')
                passed('Diagnostics shows live remote state, name and battery')
                for charging in (True, False, None):
                    fixture['remotes']['charging'] = charging
                    page.wait_for_function('() => !polling && !activityActionPending');page.evaluate('() => refresh()')
                    if charging is True:expect(page.locator('#diagnostics')).to_contain_text(' · Charging')
                    else:expect(page.locator('#diagnostics')).not_to_contain_text('Charging')
                    expect(page.locator('#diagnostics')).not_to_contain_text('Not charging')
                    expect(page.locator('#remoteStatus')).to_have_text('Paired')
                fixture['remotes'].update(connected=False,battery=-1,charging=None)
                page.wait_for_function('() => !polling && !activityActionPending');page.evaluate('() => refresh()')
                expect(page.locator('#diagnostics')).not_to_contain_text('% battery')
                expect(page.locator('#diagnostics')).not_to_contain_text('Charging')
                passed('Diagnostics adds Charging only for an explicitly charging connected remote')
                fixture['remotes'] = {'devices': [], 'bonded': False}
                page.evaluate('() => refresh()')
                page.locator('[data-tab=remotes]').click()
                expect(page.locator('#remoteStatus')).to_have_text('Not paired')
                expect(page.locator('#toggleRemote')).to_have_text('Pair remote')
                expect(page.locator('#remotes .actions button')).to_have_count(1)
                pair_gate = Event()
                fixture['remote_pair_gate'] = pair_gate
                page.locator('#toggleRemote').click()
                expect(page.locator('#remoteStatus')).to_contain_text('Looking for a nearby remote')
                expect(page.locator('#remotes progress, #remoteProgress')).to_have_count(0)
                expect(page.locator('#toggleRemote')).to_have_text('Pair remote')
                expect(page.locator('#toggleRemote')).to_be_disabled()
                pair_gate.set()
                page.wait_for_function('() => !remotePending')
                assert fixture['remote_calls'] == ['/api/remotes/pair']
                expect(page.locator('#toggleRemote')).to_have_text('Stop pairing')
                expect(page.locator('#toggleRemote')).to_be_enabled()
                passed('automatic pairing shows text feedback while the start request is pending, then offers Stop pairing, without progress')
                fixture['remotes']['devices'] = [{'name': 'Test Remote', 'address': '01:02:03:04:05:06', 'type': 0, 'rssi': -40}]
                page.evaluate('() => refresh()')
                expect(page.locator('#remotes button')).to_have_count(1)
                expect(page.locator('#remoteStatus')).to_contain_text('Looking for a nearby remote')
                expect(page.locator('#remotes progress, #remoteProgress')).to_have_count(0)
                expect(page.locator('#toggleRemote')).to_have_text('Stop pairing')
                expect(page.locator('#toggleRemote')).to_be_enabled()
                passed('nearby devices are not listed and the search offers Stop pairing instead of another pair request')
                fixture['remotes']['too_far'] = True
                page.evaluate('() => refresh()')
                expect(page.locator('#remoteStatus')).to_have_text('Remote detected. Bring it closer to Amity.')
                expect(page.locator('#remotes progress, #remoteProgress')).to_have_count(0)
                expect(page.locator('#toggleRemote')).to_have_text('Stop pairing')
                assert page.locator('#remotes').get_attribute('aria-busy') is None
                passed('weak remote displays a bring-closer prompt while search continues, without progress')
                fixture['remotes']['too_far'] = False
                page.evaluate('() => refresh()')
                expect(page.locator('#remoteStatus')).to_contain_text('Looking for a nearby remote')
                passed('stronger signal clears the proximity prompt')
                fixture['remotes'].update(scanning=False, connecting=True)
                page.evaluate('() => refresh()')
                expect(page.locator('#remoteStatus')).to_have_text('Pairing with remote…')
                expect(page.locator('#remotes progress, #remoteProgress')).to_have_count(0)
                passed('bonding displays a clear pairing message without progress')
                fixture['remotes'].update(pairing=False, connecting=False, last_error=13)
                page.evaluate('() => refresh()')
                expect(page.locator('#remoteStatus')).to_have_text('No nearby remote found. Bring it closer and try again.')
                expect(page.locator('#remotes progress, #remoteProgress')).to_have_count(0)
                expect(page.locator('#toggleRemote')).to_be_enabled()
                passed('search timeout provides a retry instruction without progress')
                for searching in ({'scanning': True}, {'connecting': True}):
                    fixture['remotes'] = {'devices': [], 'bonded': False, 'pairing': True, **searching}
                    page.evaluate('() => refresh()')
                    expect(page.locator('#toggleRemote')).to_have_text('Stop pairing')
                    expect(page.locator('#toggleRemote')).to_be_enabled()
                    stops = fixture['remote_calls'].count('/api/remotes/stop')
                    page.locator('#toggleRemote').click()
                    expect(page.locator('#remoteStatus')).to_have_text('Not paired')
                    expect(page.locator('#toggleRemote')).to_have_text('Pair remote')
                    expect(page.locator('#toggleRemote')).to_be_enabled()
                    assert fixture['remote_calls'].count('/api/remotes/stop') == stops + 1 and fixture['remote_calls'][-1] == '/api/remotes/stop'
                passed('Stop pairing ends a search or a pending connection and returns to Pair remote')
                fixture['remotes'].update(bonded=True, connected=True, last_error=0)
                page.evaluate('() => refresh()')
                expect(page.locator('#remoteStatus')).to_have_text('Paired')
                expect(page.locator('#toggleRemote')).to_have_text('Unpair remote')
                expect(page.locator('#remotes progress, #remoteProgress')).to_have_count(0)
                passed('successful automatic pairing shows Paired and restores the Unpair button without progress')
                fixture['remotes']['connected'] = False
                page.wait_for_function('() => !polling && !activityActionPending')
                page.evaluate('() => refresh()')
                expect(page.locator('#toggleRemote')).to_have_text('Unpair remote')
                expect(page.locator('#toggleRemote')).to_be_enabled()
                expect(page.locator('#remoteStatus')).to_have_text('Paired')
                expect(page.locator('#diagnostics')).to_contain_text('Remote: Waiting for remote')
                passed('paired but disconnected remote still offers Unpair remote')
                before = len(fixture['remote_calls'])
                page.once('dialog', lambda dialog: dialog.dismiss())
                page.locator('#toggleRemote').click()
                expect(page.locator('#toggleRemote')).to_be_enabled()
                expect(page.locator('#remoteStatus')).to_have_text('Paired')
                assert len(fixture['remote_calls']) == before
                passed('cancelled unpair preserves pairing')
                fixture['fail_unpair'] = True
                page.once('dialog', lambda dialog: dialog.accept())
                page.locator('#toggleRemote').click()
                expect(page.locator('#message')).to_have_text('Could not unpair')
                expect(page.locator('#toggleRemote')).to_have_text('Unpair remote')
                expect(page.locator('#toggleRemote')).to_be_enabled()
                passed('failed unpair preserves paired button state and allows retry')
                fixture['fail_unpair'] = False
                fixture['remotes']['connected'] = True
                fixture['remote_unpair_async'] = True
                unpair_gate = Event()
                fixture['remote_unpair_gate'] = unpair_gate
                page.once('dialog', lambda dialog: dialog.accept())
                page.locator('#toggleRemote').click()

                def expect_unpairing():
                    expect(page.locator('#toggleRemote')).to_have_text('Unpairing...')
                    expect(page.locator('#toggleRemote')).to_be_disabled()
                    expect(page.locator('#toggleRemote')).to_have_attribute('aria-busy', 'true')
                    expect(page.locator('#remoteStatus')).to_have_text('Unpairing...')

                expect_unpairing()
                page.evaluate('''() => {
                    window.unpairSnapshots = [];
                    window.unpairObserver = new MutationObserver(() => {
                        const button = document.querySelector('#toggleRemote');
                        unpairSnapshots.push({button: button.textContent, disabled: button.disabled,
                            status: document.querySelector('#remoteStatus').textContent});
                    });
                    unpairObserver.observe(document.querySelector('#remotes'), {
                        subtree: true, childList: true, attributes: true,
                        attributeFilter: ['disabled', 'aria-busy']
                    });
                }''')
                page.wait_for_function('() => !polling')
                page.evaluate('() => refresh()')
                expect_unpairing()
                passed('pending unpair request keeps status and disabled button at Unpairing through polling')
                unpair_gate.set()
                page.wait_for_function('() => !remotePending')
                expect_unpairing()
                passed('accepted asynchronous unpair stays busy while the old bond and link remain')
                before = len(fixture['remote_calls'])
                page.evaluate('() => document.querySelector("#toggleRemote").click()')
                assert len(fixture['remote_calls']) == before
                passed('unpairing blocks duplicate remote operations')
                for connected in (True, False):
                    fixture['remotes'] = {'devices': [], 'bonded': False, 'connected': connected,
                                          'unpairing': True, 'last_error': 13}
                    page.wait_for_function('() => !polling')
                    page.evaluate('() => refresh()')
                    expect_unpairing()
                passed('bond removal and disconnect cannot replace Unpairing with pairing, idle or error status')
                snapshots = page.evaluate('''() => {
                    unpairObserver.disconnect();
                    return unpairSnapshots;
                }''')
                assert snapshots and all(snapshot == {'button': 'Unpairing...', 'disabled': True,
                                                      'status': 'Unpairing...'} for snapshot in snapshots), snapshots
                passed('DOM changes never flash an enabled button or incorrect label/status during unpairing')
                page.reload()
                expect(page.locator('#admin')).to_be_visible()
                page.locator('[data-tab=remotes]').click()
                expect_unpairing()
                passed('reloading during unpair restores its busy state from the device')
                fixture['remotes'] = {'devices': [], 'bonded': False, 'unpairing': False}
                page.wait_for_function('() => !polling')
                page.evaluate('() => refresh()')
                expect(page.locator('#toggleRemote')).to_have_text('Pair remote')
                expect(page.locator('#toggleRemote')).to_be_enabled()
                expect(page.locator('#toggleRemote')).to_have_attribute('aria-busy', 'false')
                expect(page.locator('#remoteStatus')).to_have_text('Not paired')
                assert not fixture['remotes']['bonded']
                passed('successful unpair returns the same button to Pair remote')
                assert 'Hold Power and Volume Up' not in page.locator('#remotes').text_content()
                passed('Settings and Remote copy matches the requested wording')
                page.locator('[data-tab=matter]').click()
                expect(page.locator('#matterStatus')).to_have_text('Not paired')
                expect(page.locator('#matter .actions button')).to_have_count(1)
                page.locator('#toggleMatter').click()
                expect(page.locator('#matterStatus')).to_have_text('Ready to pair')
                expect(page.locator('#toggleMatter')).to_have_text('Stop pairing')
                expect(page.locator('#matterQr svg')).to_be_visible()
                expect(page.locator('#pairingCode')).to_have_text('Pairing code: 1234-567-8901')
                page.locator('#toggleMatter').click()
                expect(page.locator('#matterStatus')).to_have_text('Not paired')
                expect(page.locator('#toggleMatter')).to_have_text('Start pairing')
                expect(page.locator('#matterQr')).to_be_hidden()
                expect(page.locator('#pairingCode')).to_have_text('')
                passed('one Matter button opens and closes pairing and hides inactive QR codes')
                page.locator('#toggleMatter').click()
                expect(page.locator('#matterQr svg')).to_be_visible()
                fixture['matter']['commissioning'] = False
                page.wait_for_function('() => !polling && !activityActionPending')
                page.evaluate('() => refresh()')
                expect(page.locator('#matterQr')).to_be_hidden()
                expect(page.locator('#matterStatus')).to_have_text('Not paired')
                expect(page.locator('#toggleMatter')).to_have_text('Start pairing')
                passed('expired Matter pairing hides QR and resets the toggle')
                fixture['matter'] = {'commissioning': False, 'fabrics': [
                    {'index': 2, 'label': 'Home', 'vendor': 0x1349},
                    {'index': 3, 'label': '', 'vendor': 0x1384},
                    {'index': 4, 'label': 'Home', 'vendor': 0x6006}]}
                page.evaluate('() => refresh()')
                expect(page.locator('#matterStatus')).to_have_text('Connected')
                expect(page.locator('#fabricList button')).to_have_text(['Remove Apple Home', 'Remove Google Home'])
                passed('Apple Keychain is hidden alongside Apple Home and another platform')
                page.once('dialog', lambda dialog: dialog.dismiss())
                page.get_by_role('button', name='Remove Apple Home', exact=True).click()
                assert fixture['matter_remove_calls'] == []
                expect(page.locator('#fabricList button')).to_have_count(2)
                page.once('dialog', lambda dialog: dialog.accept())
                page.get_by_role('button', name='Remove Apple Home', exact=True).click()
                expect(page.locator('#fabricList button')).to_have_text(['Remove Google Home'])
                assert fixture['matter_remove_calls'] == [2]
                assert all(f['vendor'] != 0x1384 for f in fixture['matter']['fabrics'])
                passed('Apple removal requests only Home removal and lets firmware clean up Keychain')
                fixture['fail_matter_remove'] = True
                page.once('dialog', lambda dialog: dialog.accept())
                page.get_by_role('button', name='Remove Google Home', exact=True).click()
                expect(page.locator('#message')).to_have_text('Could not remove connection')
                expect(page.get_by_role('button', name='Remove Google Home', exact=True)).to_be_enabled()
                fixture['fail_matter_remove'] = False
                passed('failed connection removal retains the entry and permits retry')
                fixture['matter']['fabrics'] = [
                    {'index': 2, 'label': 'Main', 'vendor': 0x1349},
                    {'index': 5, 'label': 'Cabin', 'vendor': 0x1349},
                    {'index': 3, 'label': '', 'vendor': 0x1384}]
                page.evaluate('() => refresh()')
                expect(page.locator('#fabricList button')).to_have_text(['Remove Apple Home (Main)', 'Remove Apple Home (Cabin)'])
                page.once('dialog', lambda dialog: dialog.accept())
                page.get_by_role('button', name='Remove Apple Home (Main)', exact=True).click()
                expect(page.locator('#fabricList button')).to_have_text(['Remove Apple Home'])
                assert any(f['vendor'] == 0x1384 for f in fixture['matter']['fabrics'])
                passed('removing one of two Apple homes retains the hidden Keychain association')
                fixture['matter']['fabrics'] = [{'index': 3, 'label': '', 'vendor': 0x1384}, {'index': 9, 'label': 'Custom controller', 'vendor': 0xfff1}]
                page.evaluate('() => refresh()')
                expect(page.locator('#fabricList button')).to_have_text(['Remove Custom controller'])
                passed('orphan Keychain is hidden while unknown labeled controllers remain visible')
                fixture['matter']['fabrics'] = [{'index': 3, 'label': '', 'vendor': 0x1384}]
                page.evaluate('() => refresh()')
                expect(page.locator('#matterStatus')).to_have_text('Not paired')
                expect(page.locator('#fabricList button')).to_have_count(0)
                passed('Keychain alone never displays an association or Connected status')
                fixture['matter']['fabrics'] = []
                page.evaluate('() => refresh()')
                expect(page.locator('#matterStatus')).to_have_text('Not paired')
                expect(page.locator('#fabricList button')).to_have_count(0)
                passed('idle Remote and Matter tabs show Not paired before and after pairing operations')
                fixture['config'] = {'activities': []}
                fixture['configured'] = False
                page.reload()
                expect(page.locator('#admin')).to_be_visible()
                page.locator('#discover').click()
                expect(page.locator('#discoveryStatus')).to_have_text('Scanning HDMI…')
                fixture['status']['discovering'] = False
                fixture['status']['discovery_generation'] += 1
                page.wait_for_function('() => !polling && !activityActionPending')
                page.evaluate('() => refresh()')
                expect(page.locator('#activityList article')).to_have_count(0)
                expect(save).to_be_hidden()
                expect(page.locator('#recommendations')).to_be_visible()
                passed('current empty configuration offers scanned recommendations without overwriting saved activities')
                page.evaluate('() => { dirty=false; }')
                fixture['configured'] = True
                page.reload()
                expect(page.locator('#admin')).to_be_visible()
                page.locator('#discover').click()
                expect(page.locator('#discoveryStatus')).to_have_text('Scanning HDMI…')
                fixture['status']['discovering'] = False
                fixture['status']['discovery_generation'] += 1
                page.wait_for_function('() => !polling && !activityActionPending')
                page.evaluate('() => refresh()')
                expect(page.locator('#activityList article')).to_have_count(0)
                expect(page.locator('#recommendations')).to_be_visible()
                expect(save).to_be_hidden()
                passed('explicitly saved empty activities are preserved until recommendations are added')

                fixture['config'] = {'activities': [
                    {'name': 'Cinema', 'display': 'Offline TV', 'source': 'Player', 'audio': 'Offline Receiver',
                     'switch': {'device': 'HDMI Switch', 'input': 3}},
                    {'name': 'Games', 'display': 'Offline TV', 'source': 'Offline Console', 'audio': ''},
                    {'name': 'Quiet', 'display': 'Player', 'source': 'Player', 'audio': '   '}]}
                # Start this scenario with its own retained components. Saved
                # equipment outlives activities; observations alone do not.
                equipment.clear()
                saved_equipment.clear()
                component('New Console', 'source')
                fixture['status']['devices'] = [device for device in fixture['status']['devices']
                                                 if device['name'] in ['Player', 'Receiver', 'TV']]
                page.reload()
                expect(page.locator('#admin')).to_be_visible()
                page.wait_for_function('() => !polling')
                options = page.locator('#activities datalist option')
                def device_names():
                    return options.evaluate_all('(options) => [...new Set(options.map(option => option.value))].sort((a,b) => a.localeCompare(b))')
                expected_names = ['HDMI Switch', 'New Console', 'Offline Console', 'Offline Receiver', 'Offline TV', 'Player', 'Receiver', 'TV']
                assert device_names() == expected_names, device_names()
                passed('device suggestions merge all saved components with scan results, deduplicated without blank names')
                page.locator('#addActivity').click()
                expect(page.locator('#activityFields input[data-device-list$=DeviceNames]')).to_have_count(4)
                assert device_names() == expected_names
                page.locator('#cancelActivity').click()
                edit(1)
                expect(page.locator('#activityFields input[data-device-list$=DeviceNames]')).to_have_count(4)
                assert 'HDMI Switch' in device_names() and 'Offline Receiver' in device_names()
                page.locator('#cancelActivity').click()
                passed('create and edit dialogs offer device names saved in other activities, including optional switches')
                def role_names(role):
                    return page.locator('#'+role+'DeviceNames option').evaluate_all('(options) => options.map(option => option.value)')
                assert role_names('display') == ['Offline TV', 'Player', 'TV']
                assert role_names('source') == ['New Console', 'Offline Console', 'Player']
                assert role_names('audio') == ['Offline Receiver', 'Offline TV', 'Receiver', 'TV']
                assert role_names('switch') == ['HDMI Switch', 'Offline Receiver', 'Receiver']
                passed('equipment roles filter each dropdown and retain roles already used by saved activities')
                fixture['status']['devices'] += [
                    {'name': 'Recorder', 'type': 1, 'address': 1},
                    {'name': 'Tuner', 'type': 3, 'address': 3},
                    {'name': 'CEC Switch', 'type': 6, 'address': 4},
                    {'name': 'Video Processor', 'type': 7, 'address': 14},
                    {'name': 'Unknown', 'type': 255, 'address': 12},
                    {'name': 'Offline Receiver', 'type': 255, 'address': 13}]
                page.evaluate('() => refresh()')
                assert role_names('display') == ['Offline TV', 'Player', 'TV']
                assert role_names('source') == ['New Console', 'Offline Console', 'Player', 'Recorder', 'Tuner']
                assert role_names('audio') == ['Offline Receiver', 'Offline TV', 'Receiver', 'TV']
                assert role_names('switch') == ['CEC Switch', 'HDMI Switch', 'Offline Receiver', 'Receiver', 'Video Processor']
                assert 'Unknown' not in device_names()
                passed('recorder/tuner types are sources; switches/processors and receivers support switching; unknown devices use only saved roles')
                for open_dialog in [lambda: page.locator('#addActivity').click(), lambda: edit(1)]:
                    open_dialog()
                    lists = page.locator('#activityFields input[data-device-list]').evaluate_all('(inputs) => inputs.map(input => input.getAttribute("data-device-list"))')
                    assert lists == ['displayDeviceNames', 'sourceDeviceNames', 'audioDeviceNames', 'switchDeviceNames']
                    page.locator('#cancelActivity').click()
                passed('create and edit dialogs bind each field to its own filtered device list')
                fixture['status']['devices'] = []
                page.evaluate('() => refresh()')
                offline_names = ['HDMI Switch', 'New Console', 'Offline Console', 'Offline Receiver', 'Offline TV', 'Player']
                assert device_names() == offline_names
                page.evaluate('() => refresh()')
                assert device_names() == offline_names
                passed('saved device suggestions survive repeated refreshes with no scanned devices')
                page.locator('#activityList article').first.get_by_role('button', name='Delete', exact=True).click()
                assert device_names() == offline_names
                fixture['fail_save'] = True
                save.click()
                expect(page.locator('#message')).to_have_text('Could not save configuration')
                assert device_names() == offline_names
                fixture['fail_save'] = False
                save.click()
                expect(save).to_be_hidden()
                assert device_names() == offline_names
                passed('deleting an activity and saving does not delete its saved equipment suggestions')
                edit()
                page.locator('#activityFields input').nth(2).fill('New Console')
                page.locator('#activityForm').get_by_role('button', name='OK', exact=True).click()
                assert device_names() == offline_names
                discard.click()
                assert device_names() == offline_names
                edit()
                page.locator('#activityFields input').nth(2).fill('New Console')
                page.locator('#activityForm').get_by_role('button', name='OK', exact=True).click()
                save.click()
                expect(save).to_be_hidden()
                assert device_names() == offline_names
                page.reload()
                expect(page.locator('#admin')).to_be_visible()
                page.wait_for_function('() => !polling')
                assert device_names() == offline_names
                passed('changing activity equipment keeps both old and new saved components available after reload; discarded drafts do not change suggestions')
                route_calls = []

                def current_missing(route):
                    route_calls.append((route.request.method, route.request.url.split('/api/', 1)[-1]))
                    route.fulfill(status=404, content_type='application/json',
                                  headers={'Retry-After': '7'},
                                  body=json.dumps({'error': 'Current ' + route.request.method + ' endpoint is unavailable'}))

                def unexpected_old_route(route):
                    route_calls.append((route.request.method, route.request.url.split('/api/', 1)[-1]))
                    route.fulfill(status=200, content_type='application/json', body='{"ok":true}')

                for current_path, old_path in (
                        ('activity-editor/status', 'status'),
                        ('activity-editor/discover', 'discover')):
                    page.route('**/api/' + current_path, current_missing)
                    page.route('**/api/' + old_path, unexpected_old_route)
                    try:
                        result = page.evaluate("""async operation => {
                            const priorToken = token, priorStoredToken = sessionStorage.getItem('amityToken');
                            try { await (operation === 'status' ? request('status') : request('discover', {})); return {resolved: true}; }
                            catch (error) { return {status: error.status, message: error.message,
                                retryAfter: error.retryAfter, tokenRetained: token === priorToken,
                                storedTokenRetained: sessionStorage.getItem('amityToken') === priorStoredToken}; }
                        }""", old_path)
                        method = 'GET' if old_path == 'status' else 'POST'
                        assert result == {'status': 404, 'message': 'Current ' + method + ' endpoint is unavailable',
                                          'retryAfter': 7, 'tokenRetained': True, 'storedTokenRetained': True}, result
                        assert (method, current_path) in route_calls
                        assert not any(path == old_path for _, path in route_calls), route_calls
                    finally:
                        page.unroute('**/api/' + current_path, current_missing)
                        page.unroute('**/api/' + old_path, unexpected_old_route)
                page.wait_for_function('() => !polling')
                page.evaluate("() => message('')")
                expect(page.locator('#admin')).to_be_visible()
                passed('mapped current GET and POST 404 preserve the first error and session without retrying old routes')
                page.locator('#addActivity').click();expect(page.locator('#activityEditor')).to_be_visible()
                fixture['expired'] = True
                page.wait_for_function('() => !polling && !activityActionPending')
                page.evaluate('() => refresh().catch(reportError)')
                expect(page.locator('#login')).to_be_visible()
                expect(page.locator('#message')).to_have_text('')
                expect(page.locator('#connection')).to_have_text('')
                expect(page.locator('#activityEditor')).to_be_hidden()
                passed('expired session closes the modal and returns to login without a prompt')
                fixture['setup_error'] = True
                page.reload()
                expect(page.locator('#message')).to_contain_text('Fixture startup failed')
                expect(page.locator('#connection')).to_have_text('')
                passed('startup failure leaves header blank')
                assert not errors, errors
                passed('no browser script exceptions')
            finally:
                browser.close()
    finally:
        if fixture['save_gate']:
            fixture['save_gate'].set()
        server.shutdown()
        server.server_close()
    (ROOT / 'test-results/web-activities-ui.json').write_text(json.dumps({'passed': checks}, indent=2) + '\n')


if __name__ == '__main__':
    main()
