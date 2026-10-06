#!/usr/bin/env python3
"""Exercise firmware selection, restart verification, and recovery in the real UI."""
import json
import struct
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Thread
from playwright.sync_api import sync_playwright, expect

ROOT = Path(__file__).resolve().parents[1]


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(ROOT / 'components/web_admin/assets'), **kwargs)

    def log_message(self, *args):
        pass


def image(version):
    data = bytearray(300)
    data[0] = 0xe9
    struct.pack_into('<I', data, 32, 0xabcd5432)
    data[48:48 + len(version)] = version.encode()
    return bytes(data)


def main():
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    Thread(target=server.serve_forever, daemon=True).start()
    fixture = {'current': '0.1.27', 'boot': 'boot-1', 'uploads': [], 'fail': False,
               'stage': 'ready', 'auto_restart': True, 'target': None, 'upload_abort': False, 'legacy': False}
    checks = []
    try:
        with sync_playwright() as p:
            browser = p.chromium.launch(headless=True)
            page = browser.new_page()
            page.set_default_timeout(15000)
            errors = []
            page.on('pageerror', lambda e: errors.append(str(e)))

            def api(route):
                path = route.request.url.split('/api/')[1]
                if path == 'setup':
                    if fixture['stage'] == 'offline':
                        return route.abort('connectionrefused')
                    if fixture['legacy']:
                        return route.fulfill(json={'required': False, 'hostname': 'amity-dut'})
                    return route.fulfill(json={'required': False, 'hostname': 'amity-dut',
                        'version': fixture['current'], 'boot_id': fixture['boot'],
                        'ready': fixture['stage'] == 'ready'})
                if path == 'firmware':
                    body = route.request.post_data_buffer
                    fixture['uploads'].append((route.request.headers, body))
                    if fixture['fail']:
                        return route.fulfill(status=409, json={'error': 'Reload and confirm the downgrade.'})
                    fixture['target'] = body[48:80].split(b'\0')[0].decode()
                    if fixture['auto_restart']:
                        fixture['current'] = fixture['target']
                        fixture['boot'] += '-new'
                    if fixture['upload_abort']:
                        return route.abort('connectionreset')
                    return route.fulfill(json={'rebooting': True})
                values = {'login': {'token': 'test'}, 'config': {'version': 1, 'activities': []},
                          'status': {'version': fixture['current'], 'current_activity': -1, 'devices': []},
                          'remotes': {'devices': []}, 'matter': {'fabrics': []}}
                route.fulfill(json=values[path])

            page.route('**/api/**', api)
            def script(route):
                source = (ROOT / 'components/web_admin/assets/app.js').read_text()
                source += '\ndocument.documentElement.dataset.firmwareAsset = ' + json.dumps(fixture['current']) + ';'
                route.fulfill(body=source, content_type='text/javascript', headers={'Cache-Control': 'no-cache'})
            page.route('**/app.js', script)

            def enter():
                page.goto(f'http://127.0.0.1:{server.server_port}/')
                if page.locator('#login').is_visible():
                    page.locator('#password').fill('test')
                    page.locator('#password').press('Enter')
                expect(page.locator('#admin')).to_be_visible()
                page.locator('[data-tab=advanced]').click()

            def select(version, accept, warning, wait=True):
                before = len(fixture['uploads'])
                current = fixture['current']
                data = image(version)

                def dialog(d):
                    assert warning in d.message, d.message
                    assert current in d.message and version in d.message
                    assert len(fixture['uploads']) == before, 'uploaded before confirmation'
                    d.accept() if accept else d.dismiss()

                page.once('dialog', dialog)
                page.locator('#firmware').set_input_files(
                    {'name': 'amity.bin', 'mimeType': 'application/octet-stream', 'buffer': data})
                if wait:
                    expect(page.locator('#firmware')).to_be_enabled()
                    expect(page.locator('#firmware')).to_have_value('')
                    if accept and not fixture['fail']:
                        expect(page.locator('#message')).to_contain_text('Firmware update complete')
                        expect(page.locator('#login')).to_be_visible()
                        assert page.locator('html').get_attribute('data-firmware-asset') == version
                else:
                    expect(page.locator('#firmwareUpdate')).to_be_visible()
                    page.wait_for_function("() => firmwareUpdateRecord.phase === 'waiting'")
                assert len(fixture['uploads']) == before + int(accept)
                if accept:
                    headers, body = fixture['uploads'][-1]
                    assert body == data and headers['content-type'] == 'application/octet-stream'
                    assert headers['authorization'] == 'Bearer test'
                return fixture['uploads'][-1][0] if accept else {}

            enter()
            select('0.1.26', False, 'Downgrading is not supported. Continue at your own risk?')
            checks.append('cancelled downgrade uploads nothing and allows reselecting the same file')
            assert select('0.1.26', True, 'Downgrading is not supported')['x-amity-confirm-version'] == '0.1.27'
            checks.append('confirmed downgrade sends unchanged binary and explicit version approval')
            for version in ['0.1.26', '0.1.28', '0.1.100']:
                enter()
                assert 'x-amity-confirm-version' not in select(version, True, 'Install firmware')
            checks.append('same and newer versions verify a new healthy boot before reporting completion')
            checks.append('completion reload executes assets served by the installed firmware version')
            enter()
            select('development', False, 'cannot be compared')
            assert select('development', True, 'cannot be compared')['x-amity-confirm-version'] == '0.1.100'
            checks.append('unknown versions require explicit risk confirmation')
            enter()
            fixture['fail'] = True
            select('0.1.26', True, 'cannot be compared')
            expect(page.locator('#message')).to_have_text('Reload and confirm the downgrade.')
            expect(page.locator('#admin')).to_be_visible()
            checks.append('server rejection is readable and restores upload controls')
            before = len(fixture['uploads'])
            page.locator('#firmware').set_input_files(
                {'name': 'wrong.bin', 'mimeType': 'application/octet-stream', 'buffer': b'not firmware'})
            expect(page.locator('#message')).to_have_text('Select an Amity application firmware binary.')
            expect(page.locator('#firmware')).to_have_value('')
            assert len(fixture['uploads']) == before
            checks.append('invalid file rejected before confirmation or upload')

            fixture.update(current='0.1.41', fail=False, auto_restart=False)
            enter()
            select('0.1.41', True, 'Install firmware', wait=False)
            expect(page.locator('#firmwareUpdateStatus')).to_contain_text('Restarting Amity')
            expect(page.locator('#login')).to_be_hidden()
            expect(page.locator('#uploadProgress')).to_be_visible()
            assert page.locator('#uploadProgress').get_attribute('value') is None
            page.wait_for_timeout(2200)
            expect(page.locator('#firmwareUpdate')).to_be_visible()
            checks.append('upload acknowledgement and old same-version boot never enable login or imply completion')
            fixture['stage'] = 'offline'
            expect(page.locator('#firmwareUpdateStatus')).to_contain_text('Waiting for Amity to reconnect')
            page.reload()
            expect(page.locator('#firmwareUpdate')).to_be_visible()
            expect(page.locator('#login')).to_be_hidden()
            checks.append('reload during restart resumes waiting without exposing a stale login form')
            fixture.update(stage='starting', boot='boot-after-restart')
            expect(page.locator('#firmwareUpdateStatus')).to_contain_text('Waiting for it to be ready')
            expect(page.locator('#login')).to_be_hidden()
            checks.append('new boot still starting keeps progress visible and login unavailable')
            fixture['stage'] = 'ready'
            expect(page.locator('#message')).to_have_text(
                'Firmware update complete. Amity is running version 0.1.41. Please log in.')
            expect(page.locator('#login')).to_be_visible()
            assert page.evaluate("sessionStorage.getItem('amityFirmwareUpdate')") is None
            checks.append('healthy requested version reloads fresh assets and displays completion on login')

            fixture.update(auto_restart=True, upload_abort=True)
            enter()
            select('0.1.42', True, 'Install firmware')
            checks.append('lost upload response is resolved through boot and version verification')

            fixture.update(auto_restart=False, upload_abort=False)
            enter()
            select('0.1.43', True, 'Install firmware', wait=False)
            fixture.update(current='0.1.42', boot='rollback-boot')
            expect(page.locator('#firmwareUpdateStatus')).to_have_text(
                'Update did not complete. Amity is running version 0.1.42 instead of 0.1.43.')
            expect(page.locator('#uploadProgress')).to_be_hidden()
            expect(page.locator('#firmwareUpdateLogin')).to_be_visible()
            expect(page.locator('#login')).to_be_hidden()
            checks.append('rollback reports actual version and offers login without claiming success')
            page.locator('#firmwareUpdateLogin').click()
            expect(page.locator('#login')).to_be_visible()

            enter()
            select('0.1.43', True, 'Install firmware', wait=False)
            fixture['stage'] = 'offline'
            expect(page.locator('#firmwareUpdateStatus')).to_contain_text('Waiting for Amity to reconnect')
            page.evaluate("firmwareUpdateRecord.startedAt = Date.now() - 121000")
            expect(page.locator('#retryFirmwareWait')).to_be_visible()
            expect(page.locator('#firmwareUpdateStatus')).to_contain_text('Could not confirm the update')
            expect(page.locator('#login')).to_be_hidden()
            expect(page.locator('#uploadProgress')).to_be_hidden()
            fixture.update(current='0.1.43', boot='recovered-boot', stage='ready')
            page.locator('#retryFirmwareWait').click()
            expect(page.locator('#message')).to_contain_text('Firmware update complete')
            expect(page.locator('#login')).to_be_visible()
            checks.append('reconnection timeout explains failure and retry can confirm eventual success')
            enter()
            select('0.1.42', True, 'Downgrading is not supported', wait=False)
            fixture.update(legacy=True, current='0.1.42')
            expect(page.locator('#firmwareUpdateStatus')).to_have_text('Amity is back online. Log in to verify the firmware version.')
            expect(page.locator('#uploadProgress')).to_be_hidden()
            expect(page.locator('#firmwareUpdateLogin')).to_be_visible()
            expect(page.locator('#login')).to_be_hidden()
            page.locator('#firmwareUpdateLogin').click()
            expect(page.locator('#login')).to_be_visible()
            checks.append('older firmware without health metadata offers login without a false completion claim')
            assert not errors, errors
            browser.close()
    finally:
        server.shutdown()
        server.server_close()
    (ROOT / 'test-results/web-firmware-ui.json').write_text(json.dumps({'passed': checks}, indent=2))
    for check in checks:
        print('PASS', check)


if __name__ == '__main__':
    main()
