#!/usr/bin/env python3
"""Exercise shipped member-update controls against a localhost-only API fixture.

This verifies browser transfer/admission behavior. It does not qualify publisher
signatures, a physical update, or peer health reporting.
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
CONTROLLER, MEMBER, OTHER, JOB = (f'{n:032x}' for n in (1, 2, 3, 4))
VERSION = '0.1.73'
IMAGE = bytearray((n * 17) % 256 for n in range(6000))
IMAGE[0] = 0xe9
IMAGE[32:36] = bytes.fromhex('3254cdab')
IMAGE[48:80] = VERSION.encode().ljust(32, b'\0')
IMAGE = bytes(IMAGE)
MANIFEST = json.dumps(dict(firmware_version=VERSION, image_bytes=str(len(IMAGE)),
                          image_sha256=hashlib.sha256(IMAGE).hexdigest(),
                          signing_key_id='fixture-public-root'), sort_keys=True, separators=(',', ':'))
SIGNATURE = bytes(range(64))
BUNDLE = struct.pack('>16sIIQ', b'AMITY-UPDATE-V1\n', len(MANIFEST), len(SIGNATURE), len(IMAGE)) + MANIFEST.encode() + SIGNATURE + IMAGE


def main():
    fixture = dict(posts=[], phase='transferring', offset=0, polls=0, ready=True,
                   management_ready=False, capabilities=512, boot_release=False,
                   reject_admission=False, wrong_target=False)
    errors, checks, all_posts = [], [], []
    config = dict(version=2, controller_octopus_id=CONTROLLER, config_revision='1', presentation_revision='1',
                  octopuses=[dict(octopus_id=MEMBER, label='Bedroom Amity', supported_tentacles=1),
                             dict(octopus_id=CONTROLLER, label='Living room Amity', supported_tentacles=1)],
                  activities=[], zones=[])
    editor = dict(version=1, expected_config_revision='1', expected_presentation_revision='1',
                  activities=[], device_choices=[], quirks=[])
    status = dict(version=VERSION, hostname='amity-living-room', front_physical_address='1.0.0.0',
                  hardware='Amity', ready=True, current_activity=-1, devices=[], device_choices=[],
                  expected_boot_id='a' * 32, expected_state_revision='1')

    def passed(label):
        checks.append(label)
        print('PASS', label, flush=True)

    class Handler(SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=str(ROOT / 'components/web_admin/assets'), **kwargs)

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
            if self.path == '/api/v2/system':
                peer = {key: fixture[key] for key in ('ready', 'management_ready', 'capabilities')}
                if fixture['ready']:
                    peer['hostname'] = 'amity-bedroom'
                return self.reply(dict(octopus_id=CONTROLLER, hostname='amity-living-room',
                                       named_devices=[dict(octopus_id=CONTROLLER, hostname='amity-living-room'),
                                                      dict(octopus_id=MEMBER, hostname='amity-bedroom')],
                                       membership=dict(membership_revision='1', peers=[dict(octopus_id=MEMBER, enrolled=True, boot_id='b'*32, **peer)])))
            if self.path == '/api/v2/updates/cohort':
                fixture['polls'] += 1
                if fixture['phase'] == 'boot_checking' and fixture['boot_release']:
                    fixture['phase'] = 'complete'
                return self.reply(dict(job_id=JOB, phase=fixture['phase'], current_index='0',
                                       targets=[dict(octopus_id=OTHER if fixture['wrong_target'] else MEMBER,
                                                     offset=str(fixture['offset']))]))
            values = {'/api/setup': dict(required=False), '/api/v2/activity-editor': editor,
                      '/api/v2/activity-editor/status': status, '/api/v2/config': config,
                      '/api/remotes': dict(bonded=False, ready=True, devices=[]),
                      '/api/matter': dict(fabrics=[]), '/api/homekit': dict(available=False)}
            if self.path in values:
                return self.reply(deepcopy(values[self.path]))
            return super().do_GET()

        def do_POST(self):
            assert self.headers.get('Authorization') == 'Bearer fixture-session'
            body = json.loads(self.rfile.read(int(self.headers.get('Content-Length', '0'))))
            fixture['posts'].append((self.path, deepcopy(body)))
            all_posts.append(self.path)
            if self.path == '/api/v2/updates/cohort':
                if fixture['reject_admission']:
                    return self.reply(dict(error='This release is incompatible with the selected device.'), 409)
                assert body['manifest'] == MANIFEST and body['signature'] == SIGNATURE.hex()
                assert body['targets'] == [MEMBER]
            elif self.path == '/api/v2/updates/chunk':
                assert body['cohort_job_id'] == JOB and body['target_octopus_id'] == MEMBER
                assert body['offset'] == str(fixture['offset'])
                chunk = bytes.fromhex(body['data_hex'])
                assert 0 < len(chunk) <= 2048
                assert chunk == IMAGE[fixture['offset']:fixture['offset'] + len(chunk)]
                fixture['offset'] += len(chunk)
            elif self.path == '/api/v2/updates/install':
                assert body['cohort_job_id'] == JOB and body['target_octopus_id'] == MEMBER
                assert fixture['offset'] == len(IMAGE)
                fixture['phase'] = 'boot_checking'
            else:
                return self.reply(dict(error='Unexpected fixture mutation'), 404)
            assert len(body['request_id']) == 32
            return self.reply(dict(stage='complete'))

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    Thread(target=server.serve_forever, daemon=True).start()
    try:
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(headless=True)

            def open_page(**overrides):
                fixture.update(posts=[], phase='transferring', offset=0, polls=0, ready=True,
                               management_ready=False, capabilities=512, boot_release=False,
                               reject_admission=False, wrong_target=False)
                fixture.update(overrides)
                page = browser.new_page(viewport=dict(width=430, height=900))
                page.on('pageerror', lambda e: errors.append(str(e)))
                # Accelerate only the member reboot polling interval; execute
                # shipped scripts and their real file-input event handlers.
                page.add_init_script("sessionStorage.setItem('amityToken','fixture-session'); const delay=window.setTimeout.bind(window); window.setTimeout=(fn,ms,...args)=>delay(fn,ms===1500?30:ms,...args);")
                page.goto(f'http://127.0.0.1:{server.server_port}/')
                expect(page.locator('#admin')).to_be_visible()
                page.locator('[data-tab=amityDevices]').click()
                rows = page.locator('#managedAmityList > article')
                expect(rows).to_have_count(2)
                expect(rows.first).to_contain_text('amity-living-room')
                expect(rows.first.locator('h3')).to_have_text('amity-living-room (this device)')
                expect(rows.nth(1)).to_contain_text('amity-bedroom')
                expect(page.locator('#managedAmityList')).not_to_contain_text('Living room Amity')
                expect(page.locator('#managedAmityList')).not_to_contain_text('Bedroom Amity')
                expect(page.get_by_role('button', name='Location', exact=True)).to_have_count(0)
                for action in ('Remove', 'Replace', 'Update firmware'):
                    expect(rows.first.get_by_role('button', name=action, exact=True)).to_have_count(0)
                return page

            def choose(page, payload=BUNDLE):
                with page.expect_file_chooser() as selected:
                    page.locator('#managedAmityList').get_by_role('button', name='Update firmware', exact=True).click()
                selected.value.set_files(dict(name='amity-update.bin', mimeType='application/octet-stream', buffer=payload))

            def posts(path):
                return [body for endpoint, body in fixture['posts'] if endpoint == '/api/v2/' + path]

            page = open_page()
            expect(page.locator('#advanced input#firmware')).to_have_count(1)
            expect(page.locator('#advanced')).to_contain_text('Update Firmware')
            choose(page)
            dialog = page.locator('dialog:visible')
            expect(dialog).to_contain_text('Install firmware ' + VERSION + '?')
            assert not fixture['posts']
            passed('this main is listed without member update controls; added-device update preserves Advanced and requires deliberate install')
            dialog.get_by_role('button', name='Install firmware', exact=True).click()
            expect(dialog.get_by_role('status')).to_contain_text('Waiting for Amity to restart')
            page.wait_for_function('() => document.querySelector("dialog [role=status]")?.textContent.includes("Waiting for Amity to restart")')
            page.wait_for_timeout(150)
            assert len(posts('updates/cohort')) == 1 and len(posts('updates/install')) == 1
            assert len(posts('updates/chunk')) == 3 and fixture['offset'] == len(IMAGE)
            assert b''.join(bytes.fromhex(b['data_hex']) for b in posts('updates/chunk')) == IMAGE
            assert fixture['polls'] >= 2
            passed('exact target, manifest and signature; 2 KiB chunks contain only native image bytes')
            before = deepcopy(fixture['posts'])
            page.wait_for_timeout(120)
            assert fixture['posts'] == before
            fixture['boot_release'] = True
            expect(dialog.get_by_role('status')).to_contain_text('reported the expected firmware')
            assert fixture['posts'] == before
            passed('boot checking polls health without resending chunks or install')
            page.close()

            page = open_page(ready=False, management_ready=True, offset=2048)
            choose(page)
            dialog = page.locator('dialog:visible')
            dialog.get_by_role('button', name='Install firmware', exact=True).click()
            expect(dialog.get_by_role('status')).to_contain_text('Waiting for Amity to restart')
            assert len(posts('updates/cohort')) == 1 and posts('updates/chunk')[0]['offset'] == '2048'
            assert len(posts('updates/chunk')) == 2
            fixture['boot_release'] = True
            expect(dialog.get_by_role('status')).to_contain_text('reported the expected firmware')
            passed('management-only peer resumes only from the exact server checkpoint after explicit file choice')
            page.close()

            for label, payload in [('raw image', IMAGE), ('bad magic', b'X' + BUNDLE[1:]),
                                   ('truncated image', BUNDLE[:-1]), ('trailing image', BUNDLE + b'x'),
                                   ('bad signature length', BUNDLE[:20] + struct.pack('>I', 63) + BUNDLE[24:]),
                                   ('oversized manifest', BUNDLE[:16] + struct.pack('>I', 8193) + BUNDLE[20:])]:
                page = open_page()
                choose(page, payload)
                expect(page.locator('#message')).not_to_be_empty()
                expect(page.locator('dialog:visible')).to_have_count(0)
                assert not fixture['posts']
                passed(label + ' rejected before cohort admission')
                page.close()

            page = open_page(phase='interrupted')
            choose(page)
            dialog = page.locator('dialog:visible')
            dialog.get_by_role('button', name='Install firmware', exact=True).click()
            expect(dialog.get_by_role('status')).to_contain_text('explicit retry')
            page.wait_for_timeout(120)
            assert len(posts('updates/cohort')) == 1 and not posts('updates/chunk') and not posts('updates/install')
            passed('interrupted outcome stops and requires deliberate retry without automatic upload')
            page.close()

            page = open_page(wrong_target=True)
            choose(page)
            dialog = page.locator('dialog:visible')
            dialog.get_by_role('button', name='Install firmware', exact=True).click()
            expect(dialog.get_by_role('status')).to_contain_text('target changed')
            assert not posts('updates/chunk') and not posts('updates/install')
            passed('changed cohort target fails before any image transfer')
            page.close()

            page = open_page(reject_admission=True)
            choose(page)
            dialog = page.locator('dialog:visible')
            dialog.get_by_role('button', name='Install firmware', exact=True).click()
            expect(dialog.get_by_role('status')).to_contain_text('incompatible')
            assert not fixture['polls'] and not posts('updates/chunk') and not posts('updates/install')
            passed('server release rejection remains visible and never starts an upload')
            page.close()

            for overrides in (dict(ready=False, management_ready=False), dict(capabilities=0)):
                page = open_page(**overrides)
                expect(page.locator('#managedAmityList').get_by_role('button', name='Update firmware')).to_have_count(0)
                assert not fixture['posts']
                page.close()
            passed('offline or unsupported peer has no update control')
            assert set(all_posts) <= {'/api/v2/updates/cohort', '/api/v2/updates/chunk', '/api/v2/updates/install'}, all_posts
            assert not errors, errors
            browser.close()
    finally:
        server.shutdown()
        server.server_close()
    print(f'{len(checks)} member-update browser checks passed; no physical firmware operation performed')


if __name__ == '__main__':
    main()
