#!/usr/bin/env python3
"""Exercise the admin UI in Chromium against a device or local remote fixture."""
import argparse
import json
import http.client
import ssl
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from threading import Thread
from urllib.parse import urlsplit
from pathlib import Path
import re
from playwright.sync_api import sync_playwright, expect


def remote_fixture(output):
    """Exercise remote status transitions using the actual UI and a local API."""
    assets = Path(__file__).resolve().parents[1] / 'components/web_admin/assets'
    saved = {'ready': True, 'bonded': True, 'waiting': True, 'connected': False,
             'connecting': False, 'scanning': False, 'pairing': False,
             'unpairing': False, 'last_error': 0, 'name': 'Fixture remote',
             'battery': -1, 'charging': None, 'devices': []}
    remote = dict(saved)
    calls, checks, errors = [], [], []

    class Handler(SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=str(assets), **kwargs)
        def log_message(self, *args):
            pass
        def reply(self, value):
            data = json.dumps(value).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        def do_GET(self):
            values = {'/api/setup': {'required': False},
                      '/api/config': {'version': 1, 'activities': []},
                      '/api/status': {'version': 'test', 'ready': True, 'devices': [],
                                      'wifi_connected': True, 'wifi_rssi': -50,
                                      'wifi_channel': 6, 'current_activity': -1},
                      '/api/remotes': remote, '/api/matter': {'fabrics': []},
                      '/api/homekit': {'available': False}}
            if self.path in values:
                return self.reply(values[self.path])
            super().do_GET()
        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers.get('Content-Length', '0'))))
            if self.path == '/api/login':
                return self.reply({'token': 'fixture-session'})
            calls.append((self.path, body))
            if self.path == '/api/remotes/unpair':
                remote.update(waiting=False, unpairing=True)
            return self.reply({'ok': True})

    def passed(name):
        checks.append(name)
        print('PASS', name, flush=True)

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    Thread(target=server.serve_forever, daemon=True).start()
    try:
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(headless=True)
            try:
                page = browser.new_page()
                page.set_default_timeout(10000)
                page.on('pageerror', lambda error: errors.append(str(error)))
                page.goto(f'http://127.0.0.1:{server.server_port}/')
                page.locator('#password').fill('fixture-password')
                page.locator('#password').press('Enter')
                expect(page.locator('#admin')).to_be_visible()
                page.wait_for_function('() => !polling && remoteStatus.textContent === "Paired"')
                page.locator('[data-tab=advanced]').click()

                def refresh_remote(**changes):
                    remote.clear()
                    remote.update(saved)
                    remote.update(changes)
                    page.wait_for_function('() => !polling')
                    page.evaluate('() => refresh()')

                expect(page.locator('#diagnostics')).to_contain_text('Remote: Waiting for remote')
                page.locator('[data-tab=remotes]').click()
                expect(page.locator('#remoteStatus')).to_have_text('Paired')
                expect(page.locator('#toggleRemote')).to_have_text('Unpair remote')
                expect(page.locator('#toggleRemote')).to_be_enabled()
                passed('saved absent remote waits quietly and Unpair stays available')

                refresh_remote(waiting=False)
                expect(page.locator('#diagnostics')).to_contain_text('Remote: Waiting for remote')
                passed('saved peer stays Waiting for remote between listening attempts')

                refresh_remote(waiting=False, connecting=True)
                expect(page.locator('#diagnostics')).to_contain_text('Remote: Connecting')
                expect(page.locator('#remoteStatus')).to_have_text('Paired')
                expect(page.locator('#toggleRemote')).to_be_enabled()
                passed('actual reconnect shows Connecting only in Diagnostics')

                refresh_remote(last_error=7)
                expect(page.locator('#diagnostics')).to_contain_text('Remote: Connection failed (error 7)')
                expect(page.locator('#toggleRemote')).to_be_enabled()
                passed('real reconnect errors remain visible during background waiting')

                refresh_remote(waiting=False, connected=True, battery=64, charging=True)
                expect(page.locator('#diagnostics')).to_contain_text('Remote: Connected · Fixture remote · 64% battery · Charging')
                passed('connected remote retains name, battery and charging diagnostics')

                refresh_remote(bonded=False, waiting=False, scanning=True, pairing=True)
                expect(page.locator('#diagnostics')).to_contain_text('Remote: Scanning')
                expect(page.locator('#remoteStatus')).to_contain_text('Looking for a nearby remote')
                expect(page.locator('#toggleRemote')).to_be_disabled()
                refresh_remote(bonded=False, waiting=False, connecting=True, pairing=True)
                expect(page.locator('#remoteStatus')).to_have_text('Pairing with remote…')
                expect(page.locator('#toggleRemote')).to_be_disabled()
                refresh_remote(bonded=False, waiting=False, last_error=13)
                expect(page.locator('#remoteStatus')).to_have_text('No nearby remote found. Bring it closer and try again.')
                passed('explicit pairing keeps its search, bonding and timeout messages')

                refresh_remote()
                page.once('dialog', lambda dialog: dialog.accept())
                page.locator('#toggleRemote').click()
                expect(page.locator('#remoteStatus')).to_have_text('Unpairing...')
                expect(page.locator('#toggleRemote')).to_be_disabled()
                assert calls == [('/api/remotes/unpair', {})], calls
                refresh_remote(bonded=False, waiting=False)
                expect(page.locator('#remoteStatus')).to_have_text('Not paired')
                expect(page.locator('#diagnostics')).to_contain_text('Remote: Disconnected')
                expect(page.locator('#toggleRemote')).to_be_enabled()
                passed('Unpair works while waiting and restores the unpaired UI after cleanup')
                assert not errors, errors
                passed('no browser script exceptions')
            finally:
                browser.close()
    finally:
        server.shutdown()
        server.server_close()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps({'passed': checks}, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument('--url')
    target.add_argument('--remote-fixture', action='store_true', help='Test remote UI locally without a device')
    credentials = parser.add_mutually_exclusive_group()
    credentials.add_argument('--boot-log', type=Path, help='Legacy firmware console credential')
    credentials.add_argument('--credentials', type=Path, help='Private JSON with admin_password')
    parser.add_argument('--output', type=Path, default=Path('test-results/web-browser.json'))
    args = parser.parse_args()
    if args.remote_fixture:
        remote_fixture(args.output)
        return
    if not args.credentials and not args.boot_log:
        parser.error('--url requires --credentials or --boot-log')
    password = (json.loads(args.credentials.read_text())['admin_password'] if args.credentials else
                re.search(r'AMITY INITIAL ADMIN PASSWORD: (\S+)', args.boot_log.read_text()).group(1))
    checks = []

    def passed(name, condition=True):
        if not condition:
            raise AssertionError(name)
        checks.append(name)
        print('PASS', name)

    # Reuse a TLS connection so the second attempt reaches the device during
    # its cooldown, rather than spending the cooldown establishing TLS.
    target = urlsplit(args.url)
    connection = http.client.HTTPSConnection(target.hostname, target.port or 443,
                                             context=ssl._create_unverified_context(), timeout=15)
    def attempt(value):
        connection.request('POST', '/api/login', json.dumps({'password': value}),
                           {'Content-Type': 'application/json'})
        response = connection.getresponse()
        result = (response.status, response.getheader('Retry-After'), json.loads(response.read()))
        return result
    try:
        status, retry, result = attempt(password + '-incorrect')
        passed('API distinguishes incorrect password', status == 401 and result['error'] == 'The password is incorrect.')
        # Hashing itself takes time on the DUT. Increase the failure count
        # until enough cooldown remains to exercise the distinct 429 response.
        for _ in range(4):
            if int(retry) >= 3:
                break
            time.sleep(int(retry) + .1)
            status, retry, result = attempt(password + '-incorrect')
            assert status == 401
        assert int(retry) >= 3, 'No measurable cooldown after repeated failures'
        status, retry, result = attempt(password)
        passed('immediate correct retry reports cooldown instead of incorrect password',
               status == 429 and int(retry) > 0 and result['error'] == 'Please wait before trying again.')
        time.sleep(int(retry) + .1)
        status, _, result = attempt(password)
        passed('correct password accepted after cooldown', status == 200 and bool(result.get('token')))
        connection.request('POST', '/api/logout', '{}',
                           {'Content-Type': 'application/json', 'Authorization': 'Bearer ' + result['token']})
        response = connection.getresponse()
        response.read()
        assert response.status == 200
    finally:
        connection.close()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(headless=True)
        try:
            context = browser.new_context(ignore_https_errors=True, viewport={'width': 1280, 'height': 1000})
            page = context.new_page()
            page.set_default_timeout(40000)
            errors = []
            page.on('pageerror', lambda error: errors.append(str(error)))
            page.goto(args.url)
            expect(page.locator('#login')).to_be_visible()
            passed('login page renders', page.title() == 'Amity')
            expect(page.locator('#connection')).to_have_text('')
            passed('login greeting removed', 'Welcome home' not in page.locator('#login').text_content())
            page.locator('#password').fill(password + '-incorrect')
            with page.expect_response(lambda response: response.url.endswith('/api/login')) as rejected:
                page.locator('#password').press('Enter')
            passed('incorrect password returns 401', rejected.value.status == 401)
            expect(page.locator('#message')).to_have_text('The password is incorrect.')
            expect(page.locator('#message')).to_have_class('error')
            expect(page.locator('#login')).to_be_visible()
            expect(page.locator('#admin')).to_be_hidden()
            passed('incorrect password shows red error and stays on login')
            expect(page.locator('#loginForm button')).to_be_enabled()
            passed('login becomes available after retry delay')
            page.locator('#password').fill(password)
            page.locator('#password').press('Enter')
            expect(page.locator('#admin')).to_be_visible()
            expect(page.locator('#diagnostics')).to_contain_text('Firmware')
            page.wait_for_function('() => !polling && remoteStatus.textContent.length > 0 && matterStatus.textContent.length > 0')
            passed('Enter logs in and loads status, remote and Matter')
            expect(page.locator('#saveActivities')).to_be_hidden()
            expect(page.locator('[data-tab=devices]')).to_have_count(0)
            expect(page.locator('#devices')).to_be_hidden()
            passed('save and scanned-device results are initially hidden')
            diagnostics = page.locator('#diagnostics').text_content()
            passed('settings omit fixed hardware and CEC counters', not any(
                text in diagnostics for text in ['Amity Board', 'CEC front', 'Transmit failures', 'dropped frames']))
            passed('tabs have no redundant headings', page.locator('.tab h2').count() == 0)
            for removed in ['HOME THEATER', 'Your home theater appears', 'Volume steps are supported',
                            'Backups contain', 'Your settings are retained', 'Each activity selects']:
                passed('removed copy: ' + removed, removed not in page.locator('body').text_content())
            for tab in ['remotes', 'matter', 'advanced', 'activities']:
                page.locator(f'[data-tab="{tab}"]').click()
                expect(page.locator('#' + tab)).to_be_visible()
                passed(tab + ' tab renders')
            added = page.locator('#activityList input').count() == 0
            if added:
                page.locator('#addActivity').click()
            name = page.locator('#activityList input').first
            original_name = name.input_value()
            name.fill(original_name + ' edited')
            expect(page.locator('#saveActivities')).to_be_visible()
            passed('unsaved edits disable activity start', all(
                not button.is_enabled() for button in page.locator('[data-start]').all()))
            name.fill(original_name)
            if added:
                page.locator('#activityList article').last.get_by_role('button', name='Remove', exact=True).click()
            expect(page.locator('#saveActivities')).to_be_hidden()
            passed('reverting edits hides save again')
            page.locator('#discover').click()
            expect(page.locator('#devices')).to_be_visible()
            expect(page.locator('#discover')).to_be_enabled(timeout=120000)
            passed('manual scan completes inside Activities')
            expect(page.locator('#connection')).to_have_text('')
            passed('header clears after scanning')
            devices = page.evaluate('() => state.devices')
            assert sorted(page.locator('#deviceList .device').all_text_contents()) == sorted(d['name'] or 'Unknown' for d in devices)
            expect(page.locator('#deviceList .device svg[role=img]')).to_have_count(len(devices))
            passed('device rows contain only OSD names and role icons')
            candidates = page.evaluate('() => [...document.querySelectorAll("[data-recommendation]")].map(row => recommendations[Number(row.dataset.recommendation)])')
            configured = page.evaluate('() => config.activities')
            assert all(not any(all((candidate.get(role) or '') == (activity.get(role) or '') for role in ('display', 'source', 'audio')) for activity in configured) for candidate in candidates)
            passed('rendered recommendations exclude equivalent configured activities')
            page.locator('#devices').screenshot(path=str(args.output.with_suffix('.devices.png')))
            page.reload()
            expect(page.locator('#admin')).to_be_visible()
            page.wait_for_function('() => !polling && document.querySelectorAll("#activityList article").length === config.activities.length')
            passed('session survives reload')
            expect(page.locator('#devices')).to_be_hidden()
            passed('old scan results stay hidden after reload')
            page.screenshot(path=str(args.output.with_suffix('.png')), full_page=True)
            page.evaluate('async () => { await request("logout", {}); await refresh().catch(reportError); }')
            expect(page.locator('#login')).to_be_visible()
            expect(page.locator('#message')).to_have_text('')
            passed('expired session returns to login without a prompt')
            passed('no browser script exceptions', not errors)
        finally:
            browser.close()
    args.output.write_text(json.dumps({'passed': checks}, indent=2) + '\n')


if __name__ == '__main__':
    main()
