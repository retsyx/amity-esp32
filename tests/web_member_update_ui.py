#!/usr/bin/env python3
"""Exercise the shipped system-update UI against a localhost-only API fixture.

Checks one browser upload, autonomous per-device progress, and exact new-boot
verification. Physical transfer, signature checks, and firmware are not mocked
as passing here; those belong to the updater's host and bench tests.
"""
import hashlib
import json
import struct
from copy import deepcopy
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Thread
from playwright.sync_api import expect, sync_playwright

ROOT = Path(__file__).resolve().parents[1]
MAIN, MEMBER, OFFLINE = (f'{n:032x}' for n in (1, 2, 3))
VERSION, OLD = '0.1.73', '0.1.72'
IDENTITY, OLD_IDENTITY = 'ab' * 32, 'cd' * 32
IMAGE = bytearray((n * 17) % 256 for n in range(6000))
IMAGE[0] = 0xe9
IMAGE[32:36] = bytes.fromhex('3254cdab')
IMAGE[48:80] = VERSION.encode().ljust(32, b'\0')
IMAGE[176:208] = bytes.fromhex(IDENTITY)
IMAGE = bytes(IMAGE)
MANIFEST = json.dumps(dict(firmware_version=VERSION, image_bytes=str(len(IMAGE)),
                          image_sha256=hashlib.sha256(IMAGE).hexdigest(),
                          image_elf_sha256=IDENTITY), sort_keys=True, separators=(',', ':'))
SIGNATURE = bytes(range(64))
BUNDLE = struct.pack('>16sIIQ', b'AMITY-UPDATE-V1\n', len(MANIFEST), len(SIGNATURE), len(IMAGE)) + MANIFEST.encode() + SIGNATURE + IMAGE


def main():
    fixture = {}
    checks, errors = [], []
    config = dict(version=3, controller_octopus_id=MAIN, config_revision='1', presentation_revision='1',
                  octopuses=[dict(octopus_id=n, supported_tentacles=1) for n in (MAIN, MEMBER, OFFLINE)],
                  activities=[], zones=[])
    editor = dict(version=1, expected_config_revision='1', expected_presentation_revision='1', activities=[], device_choices=[])

    def reset(**values):
        fixture.clear()
        fixture.update(posts=[], reads=[], phase='distributing', automatic=False, ready=True, management_ready=False,
                       boot='old-boot', version=OLD, identity=OLD_IDENTITY, reject=False, offline=False,
                       auth_revoked=False, login_fail=False, login_calls=0, upload_abort=False, last_failed_upload=None, automatic_failures=[],
                       targets=[dict(octopus_id=MAIN, phase='staged', offset=str(len(IMAGE))),
                                dict(octopus_id=MEMBER, phase='uploading', offset='2048'),
                                dict(octopus_id=OFFLINE, phase='queued', offset='0')])
        fixture.update(values)

    def passed(label):
        checks.append(label)
        print('PASS', label, flush=True)

    class Handler(SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=str(ROOT / 'components/web_admin/assets'), **kwargs)

        def log_message(self, *args):
            pass

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    Thread(target=server.serve_forever, daemon=True).start()
    try:
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(headless=True)

            def api(route):
                path = route.request.url.split('/api/')[1]
                fixture['reads'].append(path)
                if path == 'login':
                    fixture['login_calls'] += 1
                    if fixture['login_fail']:
                        return route.fulfill(status=401, json=dict(error='Incorrect password.'))
                    fixture['auth_revoked'] = False
                    return route.fulfill(json=dict(token='fixture-session'))
                if path == 'firmware':
                    assert route.request.headers['authorization'] == 'Bearer fixture-session'
                    assert route.request.headers['content-type'] == 'application/octet-stream'
                    fixture['posts'].append((path, route.request.post_data_buffer))
                    if fixture['reject']:
                        return route.fulfill(status=409, json=dict(error='The release is incompatible with this Amity.'))
                    if fixture['upload_abort']:
                        fixture['last_failed_upload'] = dict(phase='failed', automatic=False, image_elf_sha256=IDENTITY,
                            image_bytes=str(len(IMAGE)), targets=[dict(octopus_id=MAIN, phase='failed', offset='2048')],
                            error='Firmware validation failed.')
                        fixture['automatic'] = True
                        return route.abort('connectionreset')
                    return route.fulfill(json=dict(staged=True))
                if route.request.method == 'POST':
                    fixture['posts'].append((path, route.request.post_data))
                    raise AssertionError('The browser must not send per-device update commands: ' + path)
                if path == 'setup':
                    if fixture['offline']:
                        return route.abort('connectionrefused')
                    return route.fulfill(json=dict(required=False, hostname='amity-main', version=fixture['version'],
                        image_elf_sha256=fixture['identity'], boot_id=fixture['boot'], ready=True, update_manifest_required=True))
                if path == 'updates/cohort':
                    assert route.request.headers['authorization'] == 'Bearer fixture-session'
                    if fixture['auth_revoked']:
                        return route.fulfill(status=401, json=dict(error='Session ended.'))
                    return route.fulfill(json=dict(phase=fixture['phase'], automatic=fixture['automatic'],
                        firmware_version=VERSION, image_elf_sha256=IDENTITY, image_bytes=str(len(IMAGE)),
                        targets=deepcopy(fixture['targets']), last_failed_upload=fixture['last_failed_upload'], automatic_failures=fixture['automatic_failures'],
                        error='Firmware validation failed.' if fixture['phase']=='failed' else ''))
                if path == 'system':
                    return route.fulfill(json=dict(octopus_id=MAIN, hostname='amity-main',
                        named_devices=[dict(octopus_id=n, hostname=name) for n, name in [(MAIN, 'amity-main'), (MEMBER, 'amity-bedroom'), (OFFLINE, 'amity-den')]],
                        membership=dict(membership_revision='1', peers=[dict(octopus_id=MEMBER, hostname='amity-bedroom', enrolled=True,
                            ready=fixture['ready'], management_ready=fixture['management_ready'], capabilities=512),
                            dict(octopus_id=OFFLINE, enrolled=True, ready=False, management_ready=False)])))
                values = {'config': config, 'activity-editor': editor,
                          'activity-editor/status': dict(version=fixture['version'], current_activity=-1, devices=[], device_choices=[],
                              expected_boot_id=fixture['boot'], expected_state_revision='1', hostname='amity-main'),
                          'remotes': dict(devices=[]), 'matter': dict(fabrics=[]), 'homekit': dict(available=False)}
                return route.fulfill(json=deepcopy(values[path]))

            def open_page(**values):
                reset(**values)
                page = browser.new_page(viewport=dict(width=430, height=900))
                page.set_default_timeout(12000)
                page.on('pageerror', lambda error: errors.append(str(error)))
                page.add_init_script("if(!sessionStorage.getItem('fixtureInitialized')){sessionStorage.setItem('amityToken','fixture-session');sessionStorage.setItem('fixtureInitialized','yes');} const delay=window.setTimeout.bind(window); window.setTimeout=(fn,ms,...args)=>delay(fn,ms===2000?30:ms,...args);")
                page.route('**/api/**', api)
                page.goto(f'http://127.0.0.1:{server.server_port}/')
                expect(page.locator('#admin')).to_be_visible()
                return page

            def upload(page, data=BUNDLE):
                page.locator('[data-tab=advanced]').click()
                page.once('dialog', lambda dialog: dialog.accept())
                page.locator('#firmware').set_input_files(dict(name='amity-update.bin', mimeType='application/octet-stream', buffer=data))

            page = open_page(ready=False, management_ready=True, automatic=True)
            page.locator('[data-tab=amityDevices]').click()
            expect(page.locator('#managedAmityList')).to_contain_text('Uploading: 34%')
            expect(page.locator('#managedAmityList button').filter(has_text='Update firmware')).to_have_count(0)
            assert fixture['posts'] == []
            passed('mismatched secondary reports automatic firmware progress without a manual update control')
            page.close()

            page = open_page(ready=False, management_ready=True, automatic=True, phase='failed',
                targets=[dict(octopus_id=MEMBER, phase='failed', offset=str(len(IMAGE)))])
            page.locator('[data-tab=amityDevices]').click()
            expect(page.locator('#managedAmityList')).to_contain_text('Firmware update failed')
            expect(page.locator('#managedAmityList')).not_to_contain_text('will retry when available')
            expect(page.locator('#managedAmityList')).not_to_contain_text('Restarting')
            passed('terminal automatic update failure is visible without promising another retry')
            page.close()

            page = open_page(ready=False, management_ready=True, automatic=True, phase='failed',
                targets=[dict(octopus_id=OFFLINE, phase='failed', offset='0')],
                automatic_failures=[dict(octopus_id=MEMBER, phase='failed')])
            page.locator('[data-tab=amityDevices]').click()
            expect(page.locator('#managedAmityList article').filter(has_text='amity-bedroom')).to_contain_text('Firmware update failed')
            passed('an earlier member failure stays visible after another member replaces the latest update job')
            page.close()

            page = open_page()
            upload(page)
            expect(page.locator('#firmwareUpdateStatus')).to_contain_text('Uploading firmware to your Amity devices')
            expect(page.locator('#firmwareUpdateTargets')).to_contain_text('amity-main — Ready to restart')
            expect(page.locator('#firmwareUpdateTargets')).to_contain_text('amity-bedroom — Uploading: 34%')
            expect(page.locator('#firmwareUpdateTargets')).to_contain_text('amity-den — Waiting')
            assert fixture['posts'] == [('firmware', BUNDLE)]
            assert page.evaluate("sessionStorage.getItem('amityToken')") == 'fixture-session'
            expect(page.locator('#login')).to_be_hidden()
            passed('one unchanged authenticated browser upload supplies every device; authenticated polling shows named progress')
            page.set_viewport_size(dict(width=320, height=900))
            assert page.evaluate('document.documentElement.scrollWidth <= innerWidth'), 'update progress overflows the mobile viewport'
            page.reload()
            expect(page.locator('#firmwareUpdateTargets')).to_contain_text('amity-bedroom — Uploading: 34%')
            assert fixture['posts'] == [('firmware', BUNDLE)]
            passed('reload resumes autonomous distribution without uploading again')
            page.evaluate('firmwareUpdateRecord.startedAt = Date.now() - 21 * 60000')
            page.wait_for_timeout(150)
            expect(page.locator('#firmwareUpdateStatus')).to_contain_text('Uploading firmware to your Amity devices')
            expect(page.locator('#retryFirmwareWait')).to_be_hidden()
            passed('a slow multi-device distribution remains visible after twenty minutes')
            fixture['targets'][1].update(phase='staged', offset=str(len(IMAGE)))
            fixture['targets'][2].update(phase='skipped', error='timeout')
            fixture['phase'] = 'activating'
            expect(page.locator('#firmwareUpdateStatus')).to_contain_text('Restarting your Amity devices')
            expect(page.locator('#firmwareUpdateTargets')).to_contain_text('will retry when available')
            assert fixture['boot'] == 'old-boot'
            expect(page.locator('#login')).to_be_hidden()
            passed('skipped devices are visible and activation acknowledgement alone never claims completion')
            fixture.update(boot='new-boot', version=VERSION, identity=IDENTITY)
            expect(page.locator('#message')).to_contain_text('Firmware update complete')
            expect(page.locator('#message')).to_contain_text('Unavailable devices will update automatically when they reconnect')
            expect(page.locator('#login')).to_be_visible()
            assert fixture['posts'] == [('firmware', BUNDLE)]
            passed('expected healthy image confirms completion after reboot and explains automatic late-device updates')
            page.close()

            page = open_page()
            upload(page)
            expect(page.locator('#firmwareUpdateStatus')).to_contain_text('Uploading firmware to your Amity devices')
            fixture.update(boot='different-boot', version=VERSION, identity='ef'*32)
            expect(page.locator('#firmwareUpdateStatus')).to_contain_text('did not report the expected firmware image')
            expect(page.locator('#firmwareUpdateLogin')).to_be_visible()
            expect(page.locator('#login')).to_be_hidden()
            passed('same version with another image never passes reboot verification')
            page.close()

            page = open_page()
            upload(page)
            expect(page.locator('#firmwareUpdateStatus')).to_contain_text('Uploading firmware to your Amity devices')
            fixture['auth_revoked'] = True
            expect(page.locator('#login')).to_be_visible()
            expect(page.locator('#firmwareUpdateStatus')).to_contain_text('Your session ended')
            assert page.evaluate("sessionStorage.getItem('amityToken')") is None
            assert page.evaluate("JSON.parse(sessionStorage.getItem('amityFirmwareUpdate')).imageIdentity") == IDENTITY
            page.reload()
            expect(page.locator('#login')).to_be_visible()
            expect(page.locator('#firmwareUpdateTargets')).to_contain_text('amity-bedroom')
            fixture['login_fail'] = True
            page.locator('#password').fill('wrong')
            page.locator('#password').press('Enter')
            expect(page.locator('#firmwareUpdateStatus')).to_have_text('Incorrect password.')
            fixture['login_fail'] = False
            page.locator('#password').fill('test-password')
            page.locator('#password').press('Enter')
            expect(page.locator('#login')).to_be_hidden()
            expect(page.locator('#firmwareUpdateStatus')).to_contain_text('Uploading firmware to your Amity devices')
            assert fixture['login_calls'] == 2 and fixture['posts'] == [('firmware', BUNDLE)]
            passed('revoked authorization preserves progress across reload and resumes after login without another upload')
            page.close()

            page = open_page(upload_abort=True)
            upload(page)
            expect(page.locator('#firmwareUpdateStatus')).to_have_text('Firmware validation failed.')
            expect(page.locator('#firmwareUpdateLogin')).to_be_visible()
            expect(page.locator('#firmwareUpdateTargets')).to_contain_text('Firmware update failed')
            assert fixture['posts'] == [('firmware', BUNDLE)]
            passed('lost upload error remains visible even after automatic catch-up replaces the current job')
            page.close()

            page = open_page()
            upload(page)
            expect(page.locator('#firmwareUpdateStatus')).to_contain_text('Uploading firmware to your Amity devices')
            page.evaluate('firmwareUpdateRecord.startedAt = Date.now() - 46 * 60000')
            expect(page.locator('#retryFirmwareWait')).to_be_visible()
            expect(page.locator('#firmwareUpdateStatus')).to_contain_text('Could not confirm the update')
            assert fixture['posts'] == [('firmware', BUNDLE)]
            passed('distribution timeout offers reconnection without replaying the firmware upload')
            page.close()

            page = open_page(reject=True)
            upload(page)
            expect(page.locator('#message')).to_have_text('The release is incompatible with this Amity.')
            expect(page.locator('#admin')).to_be_visible()
            expect(page.locator('#firmware')).to_be_enabled()
            passed('upload validation failure restores controls without starting per-device commands')
            page.close()

            assert not errors, errors
            browser.close()
    finally:
        server.shutdown()
        server.server_close()
    (ROOT / 'test-results/web-member-update-ui.json').write_text(json.dumps(dict(passed=checks), indent=2))
    print(f'{len(checks)} system-update browser checks passed; no physical firmware operation performed')


if __name__ == '__main__':
    main()
