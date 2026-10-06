#!/usr/bin/env python3
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
        'config': {'version': 1, 'activities': [{'name': 'TV', 'display': 'TV', 'source': 'Player'},
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
            if self.path == '/api/config':
                self.send_header('X-Amity-Configured', str(fixture['configured']).lower())
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if self.path == '/api/setup':
                if fixture['setup_error']:
                    return self.reply({'error': 'Fixture startup failed'}, 500)
                return self.reply({'required': False})
            if self.path.startswith('/api/') and fixture['expired']:
                return self.reply({'error': 'Login required'}, 401)
            endpoints = {'/api/config': fixture['config'], '/api/status': fixture['status'],
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
            if self.path == '/api/activity':
                fixture['activity_calls'].append(body['index'])
                fixture['status']['current_activity'] = body['index']
            if self.path == '/api/config':
                fixture['config_calls'] += 1
                if fixture['fail_save']:
                    return self.reply({'error': 'Could not save configuration'}, 500)
                fixture['config'] = body
                fixture['status']['current_activity'] = -1
                if fixture['save_gate']:
                    assert fixture['save_gate'].wait(20)
            if self.path.startswith('/api/remotes/'):
                fixture['remote_calls'].append(self.path)
            if self.path == '/api/remotes/scan':
                fixture['remotes'] = {'devices': [], 'bonded': False, 'scanning': True}
            if self.path == '/api/remotes/pair':
                assert body == {}, 'UI must request automatic pairing without selecting an address'
                if fixture['remote_pair_gate']:
                    assert fixture['remote_pair_gate'].wait(20)
                fixture['remotes'] = {'devices': [], 'bonded': False, 'pairing': True, 'scanning': True}
            if self.path == '/api/remotes/unpair':
                if fixture['fail_unpair']:
                    return self.reply({'error': 'Could not unpair'}, 500)
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
            if self.path == '/api/discover':
                if fixture['fail_discover']:
                    return self.reply({'error': 'Could not scan HDMI'}, 500)
                if fixture['discover_gate']:
                    assert fixture['discover_gate'].wait(20)
                fixture['status']['discovering'] = True
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
                def rename(value,index=0):
                    edit(index);page.locator('#activityFields input').first.fill(value)
                    page.locator('#activityForm').get_by_role('button',name='OK',exact=True).click()
                    expect(page.locator('#activityEditor')).to_be_hidden()
                def drag(source,target):
                    handle=page.locator('[data-drag]').nth(source).bounding_box()
                    row=page.locator('#activityList article').nth(target).bounding_box()
                    page.mouse.move(handle['x']+handle['width']/2,handle['y']+handle['height']/2)
                    page.mouse.down();page.mouse.move(handle['x']+handle['width']/2,row['y']+row['height']/2,steps=10);page.mouse.up()
                expect(page.locator('#activityList input')).to_have_count(0)
                expect(page.locator('#activityList article').first.locator('button')).to_have_text(['Edit','Delete','Start'])
                passed('main list contains only names, reorder handles, and Edit/Delete/Start controls')
                saved_before=deepcopy(fixture['config']);writes_before=fixture['config_calls'];activity_before=fixture['status']['current_activity']
                rename('Unwanted edit');page.locator('#activityList article').nth(1).get_by_role('button',name='Delete',exact=True).click()
                page.locator('#addActivity').click();page.locator('#activityForm').get_by_role('button',name='OK',exact=True).click();drag(0,1)
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
                page.locator('#activityFields input').first.fill('Cancelled edit')
                expect(page.locator('#editorStart')).to_have_text('Save and repair')
                expect(page.locator('#activityList .activity-name').first).to_have_text('TV')
                page.locator('#cancelActivity').click();expect(save).to_be_hidden()
                passed('modal starts and repairs saved activity; cancelling draft leaves saved list unchanged')
                edit();page.locator('#activityFields input').first.fill('Escape edit');page.keyboard.press('Escape')
                expect(page.locator('#activityEditor')).to_be_hidden();expect(save).to_be_hidden()
                passed('Escape dismisses modal without applying edits')
                rename('Changed');expect(save).to_be_visible();expect(starts.first).to_be_disabled();expect(starts.nth(1)).to_be_enabled()
                edit();page.locator('#activityFields input').first.fill('TV');before_calls=len(fixture['activity_calls'])
                expect(page.locator('#editorStart')).to_have_text('Repair');page.locator('#editorStart').click();page.wait_for_function('() => !activityActionPending')
                assert len(fixture['activity_calls'])==before_calls+1 and fixture['activity_calls'][-1]==0
                assert fixture['config']['activities'][0]['name']=='TV'
                page.locator('#cancelActivity').click();expect(page.locator('#activityList .activity-name').first).to_have_text('Changed')
                passed('modal draft reverted to saved settings can repair without persisting other staged edits')
                rename('TV');expect(save).to_be_hidden();expect(starts.first).to_be_enabled()
                passed('reverting staged edits restores saved baseline; unaffected saved activity stays runnable')
                edit();page.locator('#activityFields input').nth(3).fill('Receiver');page.locator('#activityForm').get_by_role('button',name='OK',exact=True).click();expect(save).to_be_visible()
                edit();page.locator('#activityFields input').nth(3).fill('');page.locator('#activityForm').get_by_role('button',name='OK',exact=True).click();expect(save).to_be_hidden()
                passed('missing and empty optional fields compare equally')
                page.locator('#addActivity').click();expect(page.locator('#activityEditorTitle')).to_have_text('Add activity');expect(page.locator('#activityList article')).to_have_count(2)
                page.locator('#cancelActivity').click();expect(save).to_be_hidden()
                page.locator('#addActivity').click();page.locator('#activityForm').get_by_role('button',name='OK',exact=True).click();expect(save).to_be_visible()
                page.locator('#activityList article').last.get_by_role('button',name='Delete',exact=True).click();expect(save).to_be_hidden()
                passed('Add opens draft modal; cancel adds nothing; adding then deleting restores baseline')
                drag(0,1);expect(save).to_be_visible();expect(page.locator('#activityList .activity-name')).to_have_text(['Music','TV'])
                starts.first.click();expect(starts.first).to_have_text('Repair');page.wait_for_function('() => !activityActionPending');assert fixture['activity_calls'][-1]==1
                edit(1);page.locator('#editorStart').click();expect(page.locator('#editorStart')).to_have_text('Repair');page.wait_for_function('() => !activityActionPending');assert fixture['activity_calls'][-1]==0;page.locator('#cancelActivity').click()
                passed('dragged list and modal Start target saved indices, not unsaved positions')
                page.locator('[data-drag]').nth(1).press('ArrowUp');expect(save).to_be_hidden()
                expect(page.locator('#activityList .activity-name')).to_have_text(['TV','Music'])
                passed('keyboard reorder and reversing a drag restore the persisted order')
                page.locator('#activityList article').first.get_by_role('button',name='Delete',exact=True).click();starts.first.click();expect(starts.first).to_have_text('Repair');page.wait_for_function('() => !activityActionPending');assert fixture['activity_calls'][-1]==1
                page.reload();expect(page.locator('#admin')).to_be_visible();page.wait_for_function('() => !polling && !activityActionPending')
                passed('deleting an earlier draft row never shifts the index sent to the device')
                fixture['fail_save']=True;edit();page.locator('#activityFields input').first.fill('Changed');before_calls=len(fixture['activity_calls']);page.locator('#editorStart').click()
                expect(page.locator('#activityEditorError')).to_have_text('Could not save configuration');expect(page.locator('#editorStart')).to_be_enabled()
                assert len(fixture['activity_calls'])==before_calls and fixture['config']['activities'][0]['name']=='TV'
                expect(page.locator('#activityList .activity-name').first).to_have_text('TV')
                passed('failed Save and start retains modal draft without changing the list or starting stale settings')
                fixture['fail_save']=False;page.locator('#editorStart').click();expect(page.locator('#editorStart')).to_have_text('Repair');expect(page.locator('#editorStart')).to_be_enabled()
                assert fixture['config']['activities'][0]['name']=='Changed' and fixture['activity_calls'][-1]==0
                page.locator('#cancelActivity').click();expect(save).to_be_hidden()
                rename('TV');expect(save).to_be_visible();rename('Changed');expect(save).to_be_hidden()
                passed('Save and start persists first; successful save becomes comparison baseline')
                rename('Unwanted after save');discard.click();expect(page.locator('#activityList .activity-name')).to_have_text(['Changed','Music'])
                expect(starts.first).to_have_text('Repair');expect(starts.first).to_be_enabled();expect(save).to_be_hidden();expect(discard).to_be_hidden()
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
                expect(page.locator('#recommendationEditor')).to_be_visible()
                assert page.locator('#recommendationFields input[list]').evaluate_all('(inputs) => inputs.map(input => input.getAttribute("list"))') == ['displayDeviceNames', 'sourceDeviceNames', 'audioDeviceNames', 'switchDeviceNames']
                passed('recommendation editor uses the same category-specific device lists')
                page.locator('#recommendationFields input').first.fill('My player')
                page.locator('#recommendationForm').get_by_role('button', name='OK', exact=True).click()
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
                        expect(page.locator('#diagnostics')).to_contain_text('Remote: ' + status)
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
                expect(page.locator('#toggleRemote')).to_have_text('Pair remote')
                expect(page.locator('#remotes .actions button')).to_have_count(1)
                pair_gate = Event()
                fixture['remote_pair_gate'] = pair_gate
                page.locator('#toggleRemote').click()
                expect(page.locator('#remoteStatus')).to_contain_text('Looking for a nearby remote')
                expect(page.locator('#remotes progress, #remoteProgress')).to_have_count(0)
                expect(page.locator('#toggleRemote')).to_have_text('Pairing…')
                expect(page.locator('#toggleRemote')).to_be_disabled()
                pair_gate.set()
                page.wait_for_function('() => !remotePending')
                assert fixture['remote_calls'] == ['/api/remotes/pair']
                passed('automatic pairing shows text feedback while the start request is pending, without progress')
                fixture['remotes']['devices'] = [{'name': 'Test Remote', 'address': '01:02:03:04:05:06', 'type': 0, 'rssi': -40}]
                page.evaluate('() => refresh()')
                expect(page.locator('#remotes button')).to_have_count(1)
                expect(page.locator('#remoteStatus')).to_contain_text('Looking for a nearby remote')
                expect(page.locator('#remotes progress, #remoteProgress')).to_have_count(0)
                expect(page.locator('#toggleRemote')).to_be_disabled()
                passed('nearby devices are not listed and automatic search prevents duplicate requests')
                fixture['remotes']['too_far'] = True
                page.evaluate('() => refresh()')
                expect(page.locator('#remoteStatus')).to_have_text('Remote detected. Bring it closer to Amity.')
                expect(page.locator('#remotes progress, #remoteProgress')).to_have_count(0)
                expect(page.locator('#toggleRemote')).to_be_disabled()
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
                expect(page.locator('#diagnostics')).to_contain_text('Remote: Disconnected')
                passed('paired but disconnected remote still offers Unpair remote')
                before = len(fixture['remote_calls'])
                page.once('dialog', lambda dialog: dialog.dismiss())
                page.locator('#toggleRemote').click()
                expect(page.locator('#toggleRemote')).to_be_enabled()
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
                page.once('dialog', lambda dialog: dialog.accept())
                page.locator('#toggleRemote').click()
                expect(page.locator('#toggleRemote')).to_have_text('Pair remote')
                assert not fixture['remotes']['bonded']
                passed('successful unpair returns the same button to Pair remote')
                assert 'Hold Power and Volume Up' not in page.locator('#remotes').text_content()
                passed('Settings and Remote copy matches the requested wording')
                page.locator('[data-tab=matter]').click()
                expect(page.locator('#matter .actions button')).to_have_count(1)
                page.locator('#toggleMatter').click()
                expect(page.locator('#toggleMatter')).to_have_text('Stop pairing')
                expect(page.locator('#matterQr svg')).to_be_visible()
                expect(page.locator('#pairingCode')).to_have_text('Pairing code: 1234-567-8901')
                page.locator('#toggleMatter').click()
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
                expect(page.locator('#matterStatus')).to_have_text('Not connected')
                expect(page.locator('#fabricList button')).to_have_count(0)
                passed('Keychain alone never displays an association or Connected status')
                fixture['matter']['fabrics'] = []
                page.evaluate('() => refresh()')
                expect(page.locator('#matterStatus')).to_have_text('Not connected')
                expect(page.locator('#fabricList button')).to_have_count(0)
                fixture['config'] = {'version': 1, 'activities': []}
                fixture['configured'] = False
                page.reload()
                expect(page.locator('#admin')).to_be_visible()
                page.locator('#discover').click()
                expect(page.locator('#discoveryStatus')).to_have_text('Scanning HDMI…')
                fixture['status']['discovering'] = False
                fixture['status']['discovery_generation'] += 1
                page.wait_for_function('() => !polling && !activityActionPending')
                page.evaluate('() => refresh()')
                expect(page.locator('#activityList article')).to_have_count(2)
                expect(save).to_be_visible()
                expect(page.locator('#recommendations')).to_be_hidden()
                passed('first scan defaults unconfigured activities to recommendations without saving')
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

                fixture['config'] = {'version': 1, 'activities': [
                    {'name': 'Cinema', 'display': 'Offline TV', 'source': 'Player', 'audio': 'Offline Receiver',
                     'switch': {'device': 'HDMI Switch', 'input': 3}},
                    {'name': 'Games', 'display': 'Offline TV', 'source': 'Offline Console', 'audio': ''},
                    {'name': 'Quiet', 'display': 'Player', 'source': 'Player', 'audio': '   '}]}
                fixture['status']['devices'] = [device for device in fixture['status']['devices']
                                                 if device['name'] in ['Player', 'Receiver', 'TV']]
                page.reload()
                expect(page.locator('#admin')).to_be_visible()
                page.wait_for_function('() => !polling')
                options = page.locator('#activities datalist option')
                def device_names():
                    return options.evaluate_all('(options) => [...new Set(options.map(option => option.value))].sort((a,b) => a.localeCompare(b))')
                expected_names = ['HDMI Switch', 'Offline Console', 'Offline Receiver', 'Offline TV', 'Player', 'Receiver', 'TV']
                assert device_names() == expected_names, device_names()
                passed('device suggestions merge all saved activity roles with scan results, deduplicated without blank names')
                page.locator('#addActivity').click()
                expect(page.locator('#activityFields input[list$=DeviceNames]')).to_have_count(4)
                assert device_names() == expected_names
                page.locator('#cancelActivity').click()
                edit(1)
                expect(page.locator('#activityFields input[list$=DeviceNames]')).to_have_count(4)
                assert 'HDMI Switch' in device_names() and 'Offline Receiver' in device_names()
                page.locator('#cancelActivity').click()
                passed('create and edit dialogs offer device names saved in other activities, including optional switches')
                def role_names(role):
                    return page.locator('#'+role+'DeviceNames option').evaluate_all('(options) => options.map(option => option.value)')
                assert role_names('display') == ['Offline TV', 'TV']
                assert role_names('source') == ['Offline Console', 'Player']
                assert role_names('audio') == ['Offline Receiver', 'Receiver', 'TV']
                assert role_names('switch') == ['HDMI Switch', 'Receiver']
                passed('CEC device types restrict each dropdown; known playback devices are excluded from Display even if previously configured there')
                fixture['status']['devices'] += [
                    {'name': 'Recorder', 'type': 1, 'address': 1},
                    {'name': 'Tuner', 'type': 3, 'address': 3},
                    {'name': 'CEC Switch', 'type': 6, 'address': 4},
                    {'name': 'Video Processor', 'type': 7, 'address': 14},
                    {'name': 'Unknown', 'type': 255, 'address': 12},
                    {'name': 'Offline Receiver', 'type': 255, 'address': 13}]
                page.evaluate('() => refresh()')
                assert role_names('display') == ['Offline TV', 'TV']
                assert role_names('source') == ['Offline Console', 'Player', 'Recorder', 'Tuner']
                assert role_names('audio') == ['Offline Receiver', 'Receiver', 'TV']
                assert role_names('switch') == ['CEC Switch', 'HDMI Switch', 'Receiver', 'Video Processor']
                assert 'Unknown' not in device_names()
                passed('recorder/tuner types are sources; switches/processors and receivers support switching; unknown devices use only saved roles')
                for open_dialog in [lambda: page.locator('#addActivity').click(), lambda: edit(1)]:
                    open_dialog()
                    lists = page.locator('#activityFields input[list]').evaluate_all('(inputs) => inputs.map(input => input.getAttribute("list"))')
                    assert lists == ['displayDeviceNames', 'sourceDeviceNames', 'audioDeviceNames', 'switchDeviceNames']
                    page.locator('#cancelActivity').click()
                passed('create and edit dialogs bind each field to its own filtered device list')
                fixture['status']['devices'] = []
                page.evaluate('() => refresh()')
                offline_names = ['HDMI Switch', 'Offline Console', 'Offline Receiver', 'Offline TV', 'Player']
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
                assert device_names() == ['Offline Console', 'Offline TV', 'Player']
                passed('device suggestions retain the saved baseline through staged deletion and failed save, then reflect successful deletion')
                edit()
                page.locator('#activityFields input').nth(2).fill('New Console')
                page.locator('#activityForm').get_by_role('button', name='OK', exact=True).click()
                assert device_names() == ['Offline Console', 'Offline TV', 'Player']
                discard.click()
                assert device_names() == ['Offline Console', 'Offline TV', 'Player']
                edit()
                page.locator('#activityFields input').nth(2).fill('New Console')
                page.locator('#activityForm').get_by_role('button', name='OK', exact=True).click()
                save.click()
                expect(save).to_be_hidden()
                assert device_names() == ['New Console', 'Offline TV', 'Player']
                page.reload()
                expect(page.locator('#admin')).to_be_visible()
                page.wait_for_function('() => !polling')
                assert device_names() == ['New Console', 'Offline TV', 'Player']
                passed('new saved device names replace old suggestions and persist after reload; discarded drafts do not change suggestions')
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
