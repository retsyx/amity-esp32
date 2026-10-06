#!/usr/bin/env python3
"""Exercise the firmware's admin UI in Chromium; keep credentials out of output."""
import argparse
import json
import http.client
import ssl
import time
from urllib.parse import urlsplit
from pathlib import Path
import re
from playwright.sync_api import sync_playwright, expect


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', required=True)
    credentials = parser.add_mutually_exclusive_group(required=True)
    credentials.add_argument('--boot-log', type=Path, help='Legacy firmware console credential')
    credentials.add_argument('--credentials', type=Path, help='Private JSON with admin_password')
    parser.add_argument('--output', type=Path, default=Path('test-results/web-browser.json'))
    args = parser.parse_args()
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
