#!/usr/bin/env python3
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

"""Test password onboarding on an authorized DUT with no stored admin password.

Creates a random test password in the private --credentials JSON. The fixture
operator must clear this test credential before returning the DUT to its owner.
"""
import argparse
import json
import secrets
import time
import urllib.error
import urllib.request
from pathlib import Path
from playwright.sync_api import sync_playwright, expect
from cec_web_activity import Admin, write_private


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', required=True)
    parser.add_argument('--credentials', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--expect-version', default='0.1.7')
    args = parser.parse_args()
    admin = Admin(args.url)
    checks = []

    def passed(name, condition=True):
        assert condition, name
        checks.append(name)
        print('PASS', name, flush=True)

    def rejected(name, path, body, code):
        try:
            admin.request(path, body)
        except urllib.error.HTTPError as error:
            passed(name, error.code == code)
        else:
            raise AssertionError(name)

    passed('fresh device requires setup', admin.request('/api/setup')['required'] is True)
    for path, body in [('/api/status', None), ('/api/config', None), ('/api/matter', None),
                       ('/api/remotes', None), ('/api/backup', None), ('/api/logs', None),
                       ('/api/password', {'password': 'attempted-reset'}),
                       ('/api/config', {'version': 1, 'activities': []}),
                       ('/api/reboot', {}), ('/api/firmware', {})]:
        rejected('setup locks ' + path, path, body, 401)
    rejected('empty password cannot log in', '/api/login', {'password': ''}, 401)
    for body in [{}, {'password': 12}, {'password': ''}, {'password': 'a' * 129}]:
        rejected('invalid setup password rejected', '/api/setup', body, 400)
    req = urllib.request.Request(args.url + '/api/setup', data=b'{"password":"drive-by-password"}',
                                 headers={'Content-Type': 'text/plain'})
    try:
        urllib.request.urlopen(req, context=admin.context, timeout=15)
    except urllib.error.HTTPError as error:
        passed('setup rejects cross-origin form content type', error.code == 415)
    else:
        raise AssertionError('Non-JSON setup accepted')
    passed('invalid attempts leave setup available', admin.request('/api/setup')['required'])
    password = secrets.choice('abcdefghijklmnopqrstuvwxyz')
    write_private(args.credentials, {'admin_password': password})
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(headless=True)
        try:
            context = browser.new_context(ignore_https_errors=True, viewport={'width': 390, 'height': 844})
            errors = []
            page = context.new_page()
            page.on('pageerror', lambda error: errors.append(str(error)))
            page.set_default_timeout(40000)
            page.goto(args.url)
            expect(page.locator('#onboarding')).to_be_visible()
            expect(page.locator('#login')).to_be_hidden()
            expect(page.locator('#admin')).to_be_hidden()
            expect(page.locator('#logout')).to_be_hidden()
            passed('only onboarding appears on fresh device')
            page.screenshot(path=str(args.output.with_suffix('.png')), full_page=True)
            stale = context.new_page()
            stale.set_default_timeout(40000)
            stale.goto(args.url)
            expect(stale.locator('#onboarding')).to_be_visible()
            page.locator('#setupPassword').fill(password)
            page.locator('#confirmPassword').fill('mismatched-password')
            page.locator('#setupForm button').click()
            expect(page.locator('#message')).to_contain_text('Passwords do not match')
            passed('confirmation mismatch does not configure device', admin.request('/api/setup')['required'])
            page.locator('#confirmPassword').fill(password)
            page.locator('#setupForm button').click()
            expect(page.locator('#login')).to_be_visible()
            passed('successful setup leads to login', not admin.request('/api/setup')['required'])
            stale.locator('#setupPassword').fill('second-browser-password')
            stale.locator('#confirmPassword').fill('second-browser-password')
            stale.locator('#setupForm button').click()
            expect(stale.locator('#login')).to_be_visible()
            expect(stale.locator('#message')).to_contain_text('already set')
            passed('stale onboarding cannot replace saved password')
            stale.close()
            page.locator('#password').fill(password)
            page.locator('#loginForm button').click()
            expect(page.locator('#admin')).to_be_visible()
            expect(page.locator('#diagnostics')).to_contain_text('Firmware ' + args.expect_version)
            page.wait_for_function('() => !polling')
            passed('one-character password opens admin')
            page.locator('[data-tab=advanced]').click()
            page.locator('#newPassword').fill(password * 2)
            page.locator('#changePassword button').click()
            expect(page.locator('#login')).to_be_visible()
            password *= 2
            write_private(args.credentials, {'admin_password': password})
            page.locator('#password').fill(password)
            page.locator('#loginForm button').click()
            expect(page.locator('#admin')).to_be_visible()
            passed('password change accepts two-character password')
            page.locator('#logout').click()
            expect(page.locator('#login')).to_be_visible()
            page.reload()
            expect(page.locator('#login')).to_be_visible()
            expect(page.locator('#onboarding')).to_be_hidden()
            passed('reload shows login after setup')
            passed('onboarding has no script exceptions', not errors)
        finally:
            browser.close()
    rejected('unauthenticated setup cannot overwrite password', '/api/setup', {'password': 'replacement-password'}, 409)
    admin.login(password)
    original_config = admin.request('/api/config')
    rejected('authenticated setup cannot overwrite password', '/api/setup', {'password': 'replacement-password'}, 409)
    admin.request('/api/reboot', {})
    time.sleep(10)
    passed('setup stays complete after reboot', not admin.request('/api/setup')['required'])
    rejected('reboot invalidates old session', '/api/status', None, 401)
    admin.login(password)
    passed('chosen password persists after reboot', admin.request('/api/status')['version'] == args.expect_version)
    passed('onboarding preserves configuration', admin.request('/api/config') == original_config)
    write_private(args.output, {'passed': checks})


if __name__ == '__main__':
    main()
