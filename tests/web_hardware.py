#!/usr/bin/env python3
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

"""Exercise the real HTTPS admin API, without printing credentials or downloaded logs."""
import argparse
import json
from pathlib import Path
import re
import socket
import ssl
import time
import urllib.error
import urllib.request
from urllib.parse import urlsplit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', required=True)
    parser.add_argument('--boot-log', type=Path, required=True)
    parser.add_argument('--firmware', type=Path, help='Also install this image and verify config survives OTA')
    parser.add_argument('--expect-version', help='Expected running application version after OTA')
    parser.add_argument('--timeout', type=float, default=90, help='HTTP timeout; allow more time for a serial test tunnel')
    parser.add_argument('--output', type=Path, default=Path('test-results/web-hardware.json'))
    args = parser.parse_args()
    password = re.search(r'AMITY INITIAL ADMIN PASSWORD: ([0-9a-f]+)', args.boot_log.read_text()).group(1)
    token = ''
    context = ssl._create_unverified_context()  # Dedicated fixture has a freshly generated self-signed certificate.
    checks = []

    def certificate():
        address = urlsplit(args.url)
        with socket.create_connection((address.hostname, address.port or 443), timeout=15) as raw:
            with context.wrap_socket(raw, server_hostname=address.hostname) as secured:
                return secured.getpeercert(binary_form=True)

    def interrupt_upload(firmware):
        address = urlsplit(args.url)
        header = (f'POST /api/firmware HTTP/1.1\r\nHost: {address.netloc}\r\n'
                  f'Authorization: Bearer {token}\r\nContent-Type: application/octet-stream\r\n'
                  f'Content-Length: {len(firmware)}\r\nConnection: close\r\n\r\n').encode()
        with socket.create_connection((address.hostname, address.port or 443), timeout=15) as raw:
            with context.wrap_socket(raw, server_hostname=address.hostname) as secured:
                secured.sendall(header + firmware[:4096])

    def call(path, data=None, authorized=True, binary=False):
        body = data if isinstance(data, bytes) else (json.dumps(data).encode() if data is not None else None)
        headers = {'Authorization': 'Bearer ' + token} if authorized else {}
        if body is not None:
            headers['Content-Type'] = 'application/octet-stream' if binary else 'application/json'
        request = urllib.request.Request(args.url.rstrip('/') + path, data=body, headers=headers)
        try:
            with urllib.request.urlopen(request, context=context, timeout=args.timeout) as response:
                return response.status, response.read(), dict(response.headers)
        except urllib.error.HTTPError as error:
            return error.code, error.read(), dict(error.headers)

    def ok(name, condition):
        if not condition:
            raise AssertionError(name)
        checks.append(name)
        print('PASS', name)

    def login():
        nonlocal token
        status, body, _ = call('/api/login', {'password': password}, False)
        ok('admin login', status == 200)
        token = json.loads(body)['token']

    status, body, headers = call('/', authorized=False)
    ok('HTTPS page and CSP', status == 200 and b'<title>Amity</title>' in body and 'Content-Security-Policy' in headers)
    for path in ['/api/status', '/api/config', '/api/matter', '/api/remotes', '/api/logs', '/api/backup']:
        ok('unauthenticated ' + path, call(path, authorized=False)[0] == 401)
    ok('unauthenticated OTA', call('/api/firmware', b'not firmware', False, True)[0] == 401)
    login()
    original_certificate = certificate()
    original_fabrics = json.loads(call('/api/matter')[1])['fabrics']
    original_remote = json.loads(call('/api/remotes')[1])
    status, original, _ = call('/api/config')
    ok('configuration read', status == 200)
    original_config = json.loads(original)
    changed = False
    try:
        fixtures = [
            {'version': 2, 'activities': []},
            {'version': 1, 'activities': [{'name': 'same'}, {'name': 'same'}]},
            {'version': 1, 'activities': [{'name': 'bad', 'switch': {'device': 'receiver', 'input': 256}}]},
        ]
        for i, data in enumerate(fixtures):
            ok('invalid restore ' + str(i), call('/api/restore', data)[0] == 400)
            ok('invalid restore preserves config ' + str(i), json.loads(call('/api/config')[1]) == original_config)
        ok('nested command bounded', call('/api/key', b'[' * 100 + b']' * 100)[0] == 400)
        ok('invalid firmware rejected', call('/api/firmware', b'bad image' * 100, binary=True)[0] == 400)
        ok('invalid firmware retains running application', call('/api/status')[0] == 200)
        large_config={'version':1,'activities':[{'name':str(i)+'x'*50,'display':'d'*63,'source':'s'*63,'audio':'a'*63} for i in range(16)]}
        ok('configuration exceeds one NVS page', len(json.dumps(large_config))>4000)
        ok('multi-page configuration save', call('/api/config',large_config)[0]==200)
        changed=True
        ok('multi-page configuration round-trip',json.loads(call('/api/config')[1])==large_config)
        fixture = {'version': 1, 'activities': [{'name': 'Persistence test', 'display': 'HarnessTV', 'source': 'HarnessSource', 'audio': 'HarnessSource'}]}
        ok('valid configuration save', call('/api/config', fixture)[0] == 200)
        changed = True
        ok('backup round-trip', json.loads(call('/api/backup')[1]) == fixture)
        logs = call('/api/logs')[1]
        ok('admin secret absent from log download', password.encode() not in logs)
        if args.firmware:
            image = args.firmware.read_bytes()
            version_before = json.loads(call('/api/status')[1])['version']
            interrupt_upload(image)
            ok('interrupted OTA retains running application', json.loads(call('/api/status')[1])['version'] == version_before)
            ok('interrupted OTA retains configuration', json.loads(call('/api/config')[1]) == fixture)
            ok('OTA upload accepted', call('/api/firmware', image, binary=True)[0] == 200)
        else:
            ok('reboot accepted', call('/api/reboot', {})[0] == 200)
        time.sleep(3)
        for attempt in range(45):
            try:
                if call('/', authorized=False)[0] == 200:
                    break
            except (OSError, urllib.error.URLError):
                pass
            time.sleep(1)
        login()
        ok('configuration survives OTA' if args.firmware else 'configuration survives reboot', json.loads(call('/api/config')[1]) == fixture)
        for path in ['/api/matter', '/api/remotes', '/api/status']:
            ok('healthy after reboot ' + path, call(path)[0] == 200)
        ok('TLS identity retained', certificate() == original_certificate)
        ok('Matter fabrics retained', json.loads(call('/api/matter')[1])['fabrics'] == original_fabrics)
        remote = json.loads(call('/api/remotes')[1])
        ok('remote bond retained', remote['bonded'] == original_remote['bonded'])
        if args.expect_version:
            ok('updated application version', json.loads(call('/api/status')[1])['version'] == args.expect_version)
    finally:
        if changed:
            status, _, _ = call('/api/restore', original_config)
            if status != 200:
                raise RuntimeError('Could not restore original fixture configuration')
    ok('logout', call('/api/logout', {})[0] == 200)
    ok('session revoked', call('/api/status')[0] == 401)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({'passed': checks, 'ota': bool(args.firmware)}, indent=2) + '\n')


if __name__ == '__main__':
    main()
