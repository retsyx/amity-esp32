#!/usr/bin/env python3
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

"""Verify bounded HTTP request bodies on an authorized Amity test device.

No credentials are needed and settings/pairings are not changed. The slow-login
probe deliberately occupies HTTPS administration for its 15-second body budget.
HomeKit must already be initialized with its pairing window closed; a refused
HomeKit connection is reported as skipped. Evidence files use owner-only access.
"""
import argparse
import errno
import json
import os
from pathlib import Path
import socket
import ssl
import time
from urllib.parse import urlsplit


def write_private(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    os.fchmod(descriptor, 0o600)
    with os.fdopen(descriptor, 'w') as stream:
        json.dump(value, stream, indent=2)
        stream.write('\n')


def parse_response(data):
    head, separator, body = data.partition(b'\r\n\r\n')
    if not separator:
        return None, {}, b''
    lines = head.decode('iso-8859-1').split('\r\n')
    status = int(lines[0].split(' ', 2)[1])
    headers = {}
    for line in lines[1:]:
        key, value = line.split(':', 1)
        headers[key.lower()] = value.strip()
    return status, headers, body


def read_response(connection, timeout, require_close=False):
    deadline = time.monotonic() + timeout
    data = bytearray()
    closed = False
    while time.monotonic() < deadline:
        connection.settimeout(max(.001, deadline - time.monotonic()))
        try:
            block = connection.recv(4096)
        except (ssl.SSLEOFError, ConnectionResetError):
            closed = True
            break
        except socket.timeout:
            break
        if not block:
            closed = True
            break
        data.extend(block)
        if len(data) > 65536:
            raise AssertionError('Unexpectedly large HTTP response')
        status, headers, body = parse_response(data)
        if not require_close and status is not None and 'content-length' in headers:
            if len(body) >= int(headers['content-length']):
                break
    status, headers, body = parse_response(data)
    if status is not None and 'content-length' in headers:
        assert len(body) == int(headers['content-length']), 'Incomplete HTTP response'
    return status, headers, body, closed


def request_header(method, path, host, length=None, content_type='application/json'):
    # Keep-alive is intentional: the server must close rejected unread requests.
    lines = [f'{method} {path} HTTP/1.1', f'Host: {host}', 'Connection: keep-alive']
    if length is not None:
        lines.extend([f'Content-Length: {length}', f'Content-Type: {content_type}'])
    return ('\r\n'.join(lines) + '\r\n\r\n').encode('ascii')


def decode_tlv(data):
    fields = {}
    offset = 0
    while offset < len(data):
        assert offset + 2 <= len(data), 'Truncated HomeKit TLV header'
        kind, length = data[offset:offset + 2]
        offset += 2
        assert offset + length <= len(data), 'Truncated HomeKit TLV value'
        fields[kind] = fields.get(kind, b'') + data[offset:offset + length]
        offset += length
    return fields


def run(args, evidence):
    address = urlsplit(args.url)
    assert address.scheme == 'https' and address.hostname, '--url must be an HTTPS URL'
    assert not address.username and not address.password, 'URL must not contain credentials'
    assert address.path in ('', '/') and not address.query and not address.fragment, '--url must identify the device root'
    context = ssl._create_unverified_context()  # The dedicated DUT has a self-signed identity.

    def connect():
        raw = socket.create_connection((address.hostname, address.port or 443), args.response_timeout)
        try:
            return context.wrap_socket(raw, server_hostname=address.hostname)
        except BaseException:
            raw.close()
            raise

    def passed(name, elapsed=None):
        item = {'name': name}
        if elapsed is not None:
            item['seconds'] = round(elapsed, 3)
        evidence['passed'].append(item)
        print('PASS', name, flush=True)

    def health():
        with connect() as connection:
            connection.sendall(request_header('GET', '/api/setup', address.netloc))
            status, _, body, _ = read_response(connection, args.response_timeout)
        assert status == 200, 'Boot-health request failed'
        result = json.loads(body)
        assert result.get('ready') is True and result.get('boot_id'), 'DUT is not ready'
        return result

    initial = health()
    evidence['initial_boot_id'] = initial['boot_id']
    passed('DUT reports firmware readiness before probes')

    for name, method, path, length, expected in [
        ('unauthorized POST closes without draining a large unread remainder', 'POST', '/api/status', 1048576, 401),
        ('oversized login closes without draining its unread remainder', 'POST', '/api/login', 513, 413),
        ('GET with an unread body remainder is rejected and closed', 'GET', '/api/setup', 512, 400),
        ('asset GET with an unread body remainder is rejected and closed', 'GET', '/', 512, 400),
    ]:
        with connect() as connection:
            started = time.monotonic()
            # ESP-IDF dispatches a request with Content-Length after seeing its
            # first body byte. Leave the remaining declared body unread.
            connection.sendall(request_header(method, path, address.netloc, length) + b' ')
            status, headers, body, closed = read_response(connection, args.response_timeout, True)
            elapsed = time.monotonic() - started
        assert status == expected, f'{name}: expected HTTP {expected}, got {status}'
        assert closed, f'{name}: server kept the unread request open'
        assert headers.get('connection', '').lower() == 'close', f'{name}: missing close header'
        assert isinstance(json.loads(body).get('error'), str), f'{name}: error response was lost'
        passed(name, elapsed)

    # Each send produces a complete TLS application record, but the HTTP body
    # remains incomplete. Its absolute deadline must win over continued progress.
    with connect() as connection:
        connection.sendall(request_header('POST', '/api/login', address.netloc, 512) + b'{')
        started = time.monotonic()
        next_send = started + 1
        closed = False
        received = bytearray()
        while time.monotonic() - started < args.body_deadline:
            now = time.monotonic()
            if now >= next_send:
                try:
                    connection.sendall(b' ')
                except (ssl.SSLError, ConnectionError, OSError):
                    closed = True
                    break
                next_send += 1
            connection.settimeout(.2)
            try:
                block = connection.recv(4096)
            except socket.timeout:
                continue
            except (ssl.SSLEOFError, ConnectionResetError):
                closed = True
                break
            if not block:
                closed = True
                break
            received.extend(block)
            assert len(received) <= 65536, 'Unexpected slow-login response size'
        elapsed = time.monotonic() - started
    assert closed, 'Slow login outlived the absolute body deadline'
    assert elapsed < args.body_deadline, 'Slow login closed too late'
    # A deadline shutdown may close TLS before an HTTP timeout response can be sent.
    status, _, _ = parse_response(received)
    assert status in (None, 408), f'Slow login produced unexpected HTTP {status}'
    passed('slow login is interrupted despite one-byte-per-second body progress', elapsed)

    after = health()
    assert after['boot_id'] == initial['boot_id'], 'DUT rebooted during HTTPS probes'
    passed('HTTPS recovers after a slow body without rebooting')

    hap_host = args.hap_host or address.hostname
    try:
        connection = socket.create_connection((hap_host, args.hap_port), args.response_timeout)
    except OSError as error:
        if error.errno != errno.ECONNREFUSED:
            raise
        evidence['skipped'].append('HomeKit listener is not initialized (connection refused)')
        print('SKIP HomeKit listener is not initialized', flush=True)
    else:
        with connection:
            started = time.monotonic()
            connection.sendall(request_header('POST', '/pair-setup', hap_host, 512, 'application/pairing+tlv8') + b' ')
            status, headers, body, closed = read_response(connection, args.response_timeout, True)
            elapsed = time.monotonic() - started
        assert status == 200, f'Closed HomeKit pairing returned HTTP {status}'
        fields = decode_tlv(body)
        assert fields.get(6) == b'\x02' and fields.get(7) == b'\x06', 'HomeKit pairing must be closed before this test (expected M2/Unavailable)'
        assert closed and headers.get('connection', '').lower() == 'close', 'HomeKit drained an unread forbidden pairing body'
        passed('closed HomeKit pairing rejects an unread remainder immediately with Unavailable', elapsed)

    final = health()
    assert final['boot_id'] == initial['boot_id'], 'DUT rebooted during HTTP probes'
    evidence['final_boot_id'] = final['boot_id']
    passed('DUT remains ready with an unchanged boot identifier')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', required=True, help='Authorized DUT HTTPS root URL')
    parser.add_argument('--hap-host', help='HomeKit address, if different from the HTTPS host')
    parser.add_argument('--hap-port', type=int, default=51826)
    parser.add_argument('--response-timeout', type=float, default=5, help='Maximum time for immediate rejections')
    parser.add_argument('--body-deadline', type=float, default=20, help='Maximum observed time for the 15-second login body budget')
    parser.add_argument('--output', type=Path, required=True, help='Private JSON evidence file')
    args = parser.parse_args()
    if args.response_timeout <= 0 or args.body_deadline <= 15:
        parser.error('Timeouts must be positive and the body deadline must exceed 15 seconds')
    evidence = {'passed': [], 'skipped': [], 'success': False}
    try:
        run(args, evidence)
        evidence['success'] = True
    except BaseException as error:
        evidence['failure'] = str(error)
        raise
    finally:
        write_private(args.output, evidence)


if __name__ == '__main__':
    main()
