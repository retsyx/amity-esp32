#!/usr/bin/env python3
"""Verify real Matter pairing UI and decode its QR using macOS Vision/Swift."""
import argparse
import json
import os
import re
from pathlib import Path
import subprocess
from playwright.sync_api import sync_playwright, expect
from cec_web_activity import Admin, write_private


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', required=True)
    parser.add_argument('--credentials', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    os.umask(0o077)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    password = json.loads(args.credentials.read_text())['admin_password']
    admin = Admin(args.url)
    admin.login(password)
    original = admin.request('/api/matter')
    config = admin.request('/api/config')
    checks = []

    def passed(name, condition=True):
        assert condition, name
        checks.append(name)
        print('PASS', name, flush=True)

    try:
        with sync_playwright() as p:
            browser = p.chromium.launch(headless=True)
            try:
                page = browser.new_page(ignore_https_errors=True)
                page.set_default_timeout(40000)
                errors = []
                page.on('pageerror', lambda error: errors.append(str(error)))
                page.goto(args.url)
                page.locator('#password').fill(password)
                page.locator('#password').press('Enter')
                expect(page.locator('#admin')).to_be_visible()
                page.wait_for_function('() => !polling && document.querySelector("#matterStatus").textContent.length > 0')
                assert page.locator('#standby').is_visible() == (admin.request('/api/status')['current_activity'] >= 0)
                passed('Standby removed and Scan HDMI precedes activities',
                       page.locator('#discover').bounding_box()['y'] < page.locator('#activityList').bounding_box()['y'])
                page.locator('[data-tab=advanced]').click()
                expect(page.locator('#advanced')).to_contain_text('Update Firmware')
                expect(page.locator('#advanced')).to_contain_text('Download Amity configuration for backup.')
                expect(page.locator('#backup')).to_have_text('Download')
                expect(page.locator('#advanced')).to_contain_text('Upload, and restore a previous configuration.')
                passed('Settings copy updated', all(text not in page.locator('#advanced').text_content()
                       for text in ['Free memory', 'Upload an Amity firmware binary']))
                page.locator('[data-tab=remotes]').click()
                expect(page.locator('#toggleRemote')).to_have_text(re.compile(r'^(Pair|Unpair) remote$'))
                passed('Remote copy updated', 'Hold Power and Volume Up' not in page.locator('#remotes').text_content())
                page.locator('[data-tab=matter]').click()
                expect(page.locator('#matter .actions button')).to_have_count(1)
                if admin.request('/api/matter')['commissioning']:
                    page.locator('#toggleMatter').click()
                    expect(page.locator('#toggleMatter')).to_have_text('Start pairing')
                expect(page.locator('#matterQr')).to_be_hidden()
                expect(page.locator('#pairingCode')).to_have_text('')
                page.locator('#toggleMatter').click()
                expect(page.locator('#toggleMatter')).to_have_text('Stop pairing')
                expect(page.locator('#matterQr svg')).to_be_visible()
                active = admin.request('/api/matter')
                passed('single toggle opens pairing with a QR', active['commissioning'] and bool(active['qr_modules']))
                expect(page.locator('#pairingCode')).to_contain_text(active['manual_code'][:4] + '-' + active['manual_code'][4:7] + '-' + active['manual_code'][7:])
                expected = args.output.with_suffix('.payload.json')
                write_private(expected, {'qr_payload': active['qr_payload']})
                screenshot = args.output.with_suffix('.qr.png')
                page.locator('#matterQr').screenshot(path=str(screenshot))
                subprocess.run(['swift', '-module-cache-path', str(args.output.parent / 'swift-module-cache'),
                                str(Path(__file__).with_name('decode_matter_qr.swift')),
                                str(screenshot), str(expected)], check=True)
                passed('rendered QR independently decoded with macOS Vision')
                page.locator('#toggleMatter').click()
                expect(page.locator('#toggleMatter')).to_have_text('Start pairing')
                expect(page.locator('#matterQr')).to_be_hidden()
                expect(page.locator('#pairingCode')).to_have_text('')
                closed = admin.request('/api/matter')
                passed('single toggle closes pairing and removes setup material',
                       not closed['commissioning'] and 'qr_modules' not in closed and 'qr_payload' not in closed)
                passed('no script exceptions', not errors)
            finally:
                browser.close()
        passed('pairing UI preserves fabrics and activity configuration',
               admin.request('/api/matter')['fabrics'] == original['fabrics'] and admin.request('/api/config') == config)
    finally:
        current = admin.request('/api/matter')
        if current['commissioning'] != original['commissioning']:
            admin.request('/api/matter/open' if original['commissioning'] else '/api/matter/close', {})
    write_private(args.output, {'passed': checks})


if __name__ == '__main__':
    main()
