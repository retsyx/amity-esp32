#!/usr/bin/env python3
"""Exercise manual equipment entry through the shipped activity UI."""
from copy import deepcopy
import json
import os
import tempfile
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Event, Thread
from playwright.sync_api import sync_playwright, expect

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = Path(os.environ.get('AMITY_UI_TEST_OUTPUT') or tempfile.mkdtemp(prefix='amity-components-ui-'))
MAIN, MEMBER, GROUP, BOOT, ACTIVITY = (f'{n:032x}' for n in range(1, 6))
TV, PLAYER, UNUSED = ('component:' + f'{n:032x}' for n in range(20, 23))
fixture = dict(revision=1, owners=[dict(octopus_id=MAIN, name='amity-main', supported_tentacles=1)],
               components=[], activities=[dict(activity_id=ACTIVITY, name='Watch', display=TV, source=PLAYER, audio='')],
               posts=[], fail_add=False, fail_save=False, next_component=30)


def component(value, name, role, owner=MAIN, adapter=0, side='back'):
    return dict(value=value, title=name, location='', roles=['display', 'audio'] if role == 'display' else ['audio', 'switch'] if role == 'audio' else [role],
                qualified_name=dict(consortium_id=GROUP, octopus_id=owner, tentacle_id=adapter, side=side, osd_name=name))


fixture['components'] = [component(TV, 'TV', 'display', side='front'), component(PLAYER, 'Player', 'source'), component(UNUSED, 'Unused player', 'source')]


def choices():
    result = deepcopy(fixture['components'])
    for item in result:
        name = item['qualified_name']
        owner = next(o for o in fixture['owners'] if o['octopus_id'] == name['octopus_id'])
        item['location'] = ' · '.join(([owner['name']] if len(fixture['owners']) > 1 else []) +
                                    ([f'Adapter {name["tentacle_id"] + 1}'] if owner['supported_tentacles'] > 1 else []))
    return result


def editor():
    return dict(version=1, expected_config_revision=str(fixture['revision']), expected_presentation_revision='1',
                activities=deepcopy(fixture['activities']), device_choices=choices(), component_owners=deepcopy(fixture['owners']))


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(ROOT / 'components/web_admin/assets'), **kwargs)

    def log_message(self, *args):
        pass

    def reply(self, data, status=200):
        data = json.dumps(data).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        gate = fixture.get('editor_get_gate') if self.path == '/api/activity-editor' else None
        if gate:
            fixture['editor_get_started'].set()
            assert gate.wait(10), 'The editor read must be released by the cancellation scenario'
        available = choices()
        status = dict(version='test', hostname='amity-main', ready=True, current_activity=-1, discovering=False,
                      discovery_generation=1, expected_config_revision=str(fixture['revision']), expected_presentation_revision='1',
                      expected_state_revision='1', expected_boot_id=BOOT, activity_settling=False,
                      component_owners=deepcopy(fixture['owners']), device_choices=available,
                      devices=[dict(name=c['value'], title=c['title'], location=c['location'], bus=c['qualified_name']['side'],
                                    type=0 if c['value'] == TV else 4, address=0 if c['value'] == TV else 4, physical_address=0)
                               for c in available if c['value'] in (TV, PLAYER)])
        owners = fixture['owners']
        values = {'/api/setup': dict(required=False), '/api/activity-editor': editor(), '/api/activity-editor/status': status,
                  '/api/config': dict(version=3, controller_octopus_id=MAIN, octopuses=[dict(octopus_id=o['octopus_id'], supported_tentacles=o['supported_tentacles']) for o in owners],
                                      components=[], activities=[], zones=[]),
                  '/api/system': dict(octopus_id=MAIN, hostname='amity-main', named_devices=[dict(octopus_id=o['octopus_id'], hostname=o['name']) for o in owners],
                                      membership=dict(peers=[dict(octopus_id=o['octopus_id'], enrolled=True, ready=True) for o in owners if o['octopus_id'] != MAIN])),
                  '/api/remotes': dict(devices=[], bonded=False), '/api/matter': dict(fabrics=[]), '/api/homekit': dict(available=False)}
        if self.path in values:
            return self.reply(values[self.path])
        return super().do_GET()

    def do_POST(self):
        data = json.loads(self.rfile.read(int(self.headers.get('Content-Length', '0'))))
        if self.path == '/api/login':
            return self.reply(dict(token='component-fixture'))
        fixture['posts'].append((self.path, deepcopy(data)))
        if self.path in ('/api/activity-editor/components', '/api/activity-editor'):
            assert data['expected_config_revision'] == str(fixture['revision'])
            assert data['expected_presentation_revision'] == '1'
            assert len(data['request_id']) == 32
        if self.path == '/api/activity-editor/components':
            assert set(data) == {'request_id', 'expected_config_revision', 'expected_presentation_revision', 'octopus_id', 'tentacle_id', 'osd_name', 'side', 'role'}
            if fixture['fail_add']:
                return self.reply(dict(error='Could not add component'), 500)
            owner = next(o for o in fixture['owners'] if o['octopus_id'] == data['octopus_id'])
            assert 0 <= data['tentacle_id'] < owner['supported_tentacles']
            assert data['side'] in ('front', 'back') and data['role'] in ('display', 'source', 'audio', 'switch')
            assert 1 <= len(data['osd_name'].encode()) <= 14 and all(32 <= ord(c) < 127 for c in data['osd_name'])
            value = 'component:' + f'{fixture["next_component"]:032x}'
            fixture['next_component'] += 1
            fixture['components'].append(component(value, data['osd_name'], data['role'], data['octopus_id'], data['tentacle_id'], data['side']))
            fixture['revision'] += 1
            return self.reply(dict(operation_id=data['request_id'], stage='applied', saved=True))
        if self.path == '/api/activity-editor':
            if fixture['fail_save']:
                return self.reply(dict(error='Could not save activities'), 500)
            fixture['activities'] = deepcopy(data['activities'])
            for i, row in enumerate(fixture['activities']):
                row.setdefault('activity_id', f'{100 + i:032x}')
                for role in ('display', 'source', 'audio'):
                    assert not row[role] or any(c['value'] == row[role] and role in c['roles'] for c in fixture['components'])
            fixture['revision'] += 1
            return self.reply(dict(operation_id=data['request_id'], stage='applied', saved=True))
        return self.reply(dict(error='Unexpected mutation'), 404)


def main():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    Thread(target=server.serve_forever, daemon=True).start()
    checks = []

    def passed(label):
        checks.append(label)
        print('PASS', label, flush=True)

    try:
        with sync_playwright() as p:
            browser = p.chromium.launch(headless=True)
            try:
                page = browser.new_page(viewport=dict(width=430, height=900))
                page.set_default_timeout(10000)
                errors = []
                page.on('pageerror', lambda error: errors.append(str(error)))
                page.goto(f'http://127.0.0.1:{server.server_port}/')
                page.locator('#password').fill('fixture')
                page.locator('#password').press('Enter')
                expect(page.locator('#admin')).to_be_visible()
                page.wait_for_function('() => !polling')

                def source():
                    return page.locator('#activityFields [data-device-role=source]')

                def capture(name):
                    for width in (390, 816):
                        page.set_viewport_size(dict(width=width, height=900))
                        page.screenshot(path=str(OUTPUT / f'{name}-{width}.png'), full_page=True)
                    page.set_viewport_size(dict(width=430, height=900))

                def add_dialog(name, screenshot=None):
                    source().fill(name)
                    if screenshot:
                        capture(screenshot)
                    page.locator('.device-options:visible').get_by_role('option', name='Add component…', exact=True).click()
                    d = page.locator('#componentEditor')
                    expect(d).to_be_visible()
                    return d

                def cancel_component(d):
                    d.get_by_role('button', name='Cancel', exact=True).click()
                    expect(d).to_be_hidden()

                page.locator('#addActivity').click()
                expect(page.locator('#activityForm button[type=submit]')).to_have_text('Add')
                source().click()
                dropdown = page.locator('.device-options:visible')
                expect(dropdown.get_by_role('option', name='Unused player', exact=True)).to_be_visible()
                dropdown.get_by_role('option', name='Player', exact=True).click()
                expect(source()).to_have_value('Player')
                source().fill('Unused')
                source().press('ArrowDown')
                page.wait_for_function('() => !polling')
                page.evaluate('() => refresh()')
                source().press('Enter')
                expect(source()).to_have_value('Unused player')
                passed('known and unused saved components stay selectable by mouse and keyboard through polling')

                # Hold the real editor read so cancellation happens while the
                # exported add operation is waiting, then await that operation
                # itself rather than guessing when its continuation has run.
                page.evaluate('''() => {
                    window.componentAddOriginal = window.amityInterface.addComponent;
                    window.componentAddCompletions = 0;
                    window.amityInterface.addComponent = async (...args) => {
                        try { return await window.componentAddOriginal(...args); }
                        finally { window.componentAddCompletions++; }
                    };
                }''')
                gate = Event()
                fixture.update(editor_get_gate=gate, editor_get_started=Event())
                posts_before = len(fixture['posts'])
                activity_count = len(fixture['activities'])
                try:
                    page.locator('#activityFields input').first.fill('Cancelled parent')
                    source().fill('CancelledLoad')
                    with page.expect_request(lambda request: request.method == 'GET' and request.url.endswith('/api/activity-editor')):
                        page.locator('.device-options:visible').get_by_role('option', name='Add component…', exact=True).click()
                    assert fixture['editor_get_started'].wait(5), 'The component editor lookup must reach the HTTP fixture'
                    page.locator('#cancelActivity').click()
                    expect(page.locator('#activityEditor')).to_be_hidden()
                    gate.set()
                    page.wait_for_function('() => window.componentAddCompletions === 1')
                finally:
                    gate.set()
                    fixture['editor_get_gate'] = None
                expect(page.locator('#componentEditor')).to_be_hidden()
                expect(page.locator('#activityEditor')).to_be_hidden()
                assert len(fixture['posts']) == posts_before and len(fixture['activities']) == activity_count
                page.evaluate('() => { window.amityInterface.addComponent = window.componentAddOriginal; }')
                page.locator('#addActivity').click()
                expect(page.locator('#activityFields input').first).to_have_value('New activity')
                expect(source()).to_have_value('')
                passed('cancelling the parent during the initial component lookup prevents a late child dialog and leaves a fresh next draft')

                d = add_dialog('OfflineBox', 'equipment-dropdown')
                expect(d.locator('#componentOwner')).to_be_hidden()
                expect(d.locator('#componentAdapter')).to_be_hidden()
                expect(d.locator('#componentOsdName')).to_have_value('OfflineBox')
                expect(d.locator('#componentSide option')).to_have_text(['Source', 'Display'])
                assert d.locator('#componentSide option').evaluate_all('(nodes) => nodes.map(n => n.value)') == ['back', 'front']
                expect(d.locator('#componentSide')).to_have_value('back')
                assert d.locator('#componentOwner').evaluate('(e) => e.tagName') == 'SELECT'
                assert d.locator('#componentAdapter').evaluate('(e) => e.tagName') == 'SELECT'
                capture('add-component-single-owner')
                before = len(fixture['posts'])
                cancel_component(d)
                assert len(fixture['posts']) == before
                passed('arbitrary OSD opens prefilled dialog; single Amity and adapter selectors are omitted; cancel saves nothing')

                d = add_dialog('ParentDraft')
                parent_name = page.locator('#activityFields input').first.input_value()
                d.locator('#componentOsdName').fill('DiscardThis')
                box = d.bounding_box()
                page.mouse.click(box['x'] + 5, box['y'] + 5)
                expect(d).to_be_visible()
                page.mouse.move(box['x'] + 5, box['y'] + 5)
                page.mouse.down()
                page.mouse.move(5, 5, steps=5)
                page.mouse.up()
                expect(d).to_be_visible()
                page.mouse.click(5, 5)
                expect(d).to_be_hidden()
                expect(page.locator('#activityEditor')).to_be_visible()
                expect(page.locator('#activityFields input').first).to_have_value(parent_name)
                expect(source()).to_have_value('ParentDraft')
                assert len(fixture['posts']) == before
                d = add_dialog('ParentDraft')
                expect(d.locator('#componentOsdName')).to_have_value('ParentDraft')
                cancel_component(d)
                passed('component backdrop cancels only the top dialog without saving; dialog padding and inside-to-outside drags preserve the draft')

                d = add_dialog('OfflineBox')
                fixture['fail_add'] = True
                d.get_by_role('button', name='Add', exact=True).click()
                expect(d.locator('#componentEditorError')).to_have_text('Could not add component')
                expect(d.locator('#componentOsdName')).to_have_value('OfflineBox')
                expect(d.get_by_role('button', name='Add', exact=True)).to_be_enabled()
                fixture['fail_add'] = False
                d.get_by_role('button', name='Add', exact=True).click()
                expect(d).to_be_hidden()
                expect(source()).to_have_value('OfflineBox')
                assert len(fixture['components']) == 4 and fixture['components'][-1]['title'] == 'OfflineBox'
                assert fixture['activities'][0]['source'] == PLAYER
                page.locator('#cancelActivity').click()
                page.reload()
                expect(page.locator('#admin')).to_be_visible()
                page.wait_for_function('() => !polling')
                assert 'OfflineBox' in page.locator('#sourceDeviceNames option').evaluate_all('(nodes) => nodes.map(n => n.value)')
                assert not any(path.endswith('/discover') for path, _ in fixture['posts'])
                passed('failed add retains edits; successful add persists without activity save, observation, or scan and survives reload')

                page.locator('#addActivity').click()
                page.locator('#activityFields input').first.fill('Offline activity')
                page.locator('#activityFields [data-device-role=display]').fill('TV')
                source().fill('OfflineBox')
                d = add_dialog('RemoteBox')
                fixture['owners'].append(dict(octopus_id=MEMBER, name='amity-bedroom', supported_tentacles=4))
                page.wait_for_function('() => !polling')
                page.evaluate('() => refresh()')
                owner, adapter = d.locator('#componentOwner'), d.locator('#componentAdapter')
                expect(owner).to_be_visible()
                expect(page.locator('#activityFields [data-device-role=display]')).to_have_value('TV · amity-main')
                expect(owner.locator('option')).to_have_text(['amity-main', 'amity-bedroom'])
                owner.select_option(MEMBER)
                expect(adapter).to_be_visible()
                expect(d.get_by_role('combobox', name='Adapter', exact=True)).to_have_count(1)
                expect(d.get_by_role('combobox', name='Side', exact=True)).to_have_count(1)
                expect(adapter.locator('option')).to_have_text(['1', '2', '3', '4'])
                adapter.select_option(label='3')
                expect(adapter).to_have_value('2')
                d.locator('#componentSide').select_option('back')
                expect(d.locator('#componentOsdName')).to_have_value('RemoteBox')
                capture('add-component-multiple-owners')
                passed('membership changes reveal current fixed owner choices in an open dialog and select the owner firmware adapter capacity')

                for width in (320, 430, 816):
                    page.set_viewport_size(dict(width=width, height=900))
                    assert d.evaluate('(e) => e.scrollWidth <= e.clientWidth + 1')
                    page.screenshot(path=str(OUTPUT / f'add-component-{width}.png'), full_page=True)
                page.set_viewport_size(dict(width=430, height=900))
                expect(adapter).to_have_value('2')
                d.get_by_role('button', name='Add', exact=True).click()
                expect(d).to_be_hidden()
                expect(source()).to_have_value('RemoteBox · amity-bedroom · Adapter 3')
                capture('activity-owning-amity')
                created = fixture['components'][-1]
                assert created['qualified_name'] == dict(consortium_id=GROUP, octopus_id=MEMBER, tentacle_id=2, side='back', osd_name='RemoteBox')
                passed('manual remote component retains exact owning Amity, zero-based adapter and OSD while the UI uses friendly names')

                page.locator('#activityForm').get_by_role('button', name='Add', exact=True).click()
                page.locator('#saveActivities').click()
                expect(page.locator('#saveActivities')).to_be_hidden()
                assert fixture['activities'][-1]['source'] == created['value']
                page.reload()
                expect(page.locator('#admin')).to_be_visible()
                page.wait_for_function('() => !polling')
                page.locator('#activityList article').last.get_by_role('button', name='Edit', exact=True).click()
                expect(page.locator('#activityForm button[type=submit]')).to_have_text('OK')
                expect(source()).to_have_value('RemoteBox · amity-bedroom · Adapter 3')
                assert 'OfflineBox · amity-main' in page.locator('#sourceDeviceNames option').evaluate_all('(nodes) => nodes.map(n => n.value)')
                assert not any(path.endswith('/discover') for path, _ in fixture['posts'])
                passed('manual choices save exact component references and both previous and new equipment display their owning Amity after membership changes')

                d = add_dialog('RemoteBox')
                d.locator('#componentOwner').select_option(MEMBER)
                d.locator('#componentAdapter').select_option(label='3')
                d.locator('#componentSide').select_option('front')
                d.get_by_role('button', name='Add', exact=True).click()
                expect(d).to_be_hidden()
                expect(source()).to_have_value('RemoteBox · amity-bedroom · Adapter 3 · Display')
                opposite = fixture['components'][-1]
                assert opposite['qualified_name']['side'] == 'front' and opposite['value'] != created['value']
                page.locator('#activityForm').get_by_role('button', name='OK', exact=True).click()
                page.locator('#saveActivities').click()
                expect(page.locator('#saveActivities')).to_be_hidden()
                assert fixture['activities'][-1]['source'] == opposite['value']
                assert any(value.endswith(' · Source') for value in page.locator('#sourceDeviceNames option').evaluate_all('(nodes) => nodes.map(n => n.value)'))
                assert errors == [], errors
                passed('equal OSD names on opposing HDMI connections display distinct friendly locations and save the intended component')
            finally:
                browser.close()
    finally:
        server.shutdown()
        server.server_close()
    (OUTPUT / 'results.json').write_text(json.dumps(dict(passed=checks), indent=2) + '\n')
    print('Artifacts:', OUTPUT)


if __name__ == '__main__':
    main()
