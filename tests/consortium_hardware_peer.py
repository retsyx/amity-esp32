#!/usr/bin/env python3
"""Independent BlueZ candidate and mutual-TLS peer for a physical consortium bench.

Run only on an explicitly reserved harness. It registers its own GATT application
and advertisement, never removes bonds or changes adapter state. Private fixture state uses mode0600 files. No real CEC actuator is impersonated: owner
snapshots report only unconfigured monitor ports. Requires dbus-next+cryptography.
"""
import argparse
import asyncio
import datetime
import hashlib
import json
import os
import re
from pathlib import Path
import secrets
import socket
import ssl
import struct
import time

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

SERVICE = 'd6357cd0-0116-4681-a7b5-cb9d090ca311'
RECORD = 'd6357cd0-0116-4681-a7b5-cb9d090ca312'
APP = '/amity/consortium'
ADV = '/amity/consortium_advertisement'
# The fixture implements authenticated enrollment, hashed setup metadata,
# observation reports and removal. It never owns a CEC transmitter.
REQUIRED_CAPABILITIES = 64 | 8192 | 131072 | 262144
KNOWN_CAPABILITIES = REQUIRED_CAPABILITIES | 2 | 8 | 16384 | 65536
TLS_PROFILES = ['amity-consortium-v1.hardware', 'amity-consortium-v1.extended']


def hello_hardware_fields(profile):
    # A single synthetic monitor slot, with no CEC actuator. This metadata does
    # not assert the presence of a physical HDMI adapter on the harness.
    return {'supported_tentacles': 1, 'configured_tentacles_mask': 1} if profile == TLS_PROFILES[0] else {}


def encoded(value):
    return json.dumps(value, ensure_ascii=False, separators=(',', ':')).encode()


def digest(value):
    return hashlib.sha256(value).hexdigest()


class Retired(Exception):
    """The exact authenticated terminal transaction has completed."""


async def wait_window(seconds, stop_file):
    deadline = asyncio.get_running_loop().time() + seconds
    while asyncio.get_running_loop().time() < deadline and not (stop_file and stop_file.exists()):
        await asyncio.sleep(min(.5, max(0, deadline - asyncio.get_running_loop().time())))


def atomic_private(path, value):
    tmp = path.with_suffix(path.suffix + '.pending')
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    os.fchmod(fd, 0o600)
    with os.fdopen(fd, 'wb') as out:
        out.write(value)
        out.flush()
        os.fsync(out.fileno())
    os.replace(tmp, path)


def new_credential(identity):
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'o-' + identity + '.amity.invalid'),
                      x509.NameAttribute(NameOID.ORGANIZATION_NAME, 'Amity Consortium')])
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name)
            .public_key(key.public_key()).serial_number(secrets.randbits(120) | 1)
            .not_valid_before(datetime.datetime(2024, 1, 1, tzinfo=datetime.timezone.utc))
            .not_valid_after(datetime.datetime(2050, 1, 1, tzinfo=datetime.timezone.utc))
            .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
            .add_extension(x509.KeyUsage(True, False, False, False, False, False, False, False, False), critical=True)
            .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.CLIENT_AUTH, ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
            .add_extension(x509.SubjectAlternativeName([x509.DNSName('o-' + identity + '.amity.invalid')]), critical=False)
            .sign(key, hashes.SHA256()))
    return key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                             serialization.NoEncryption()).decode(), cert.public_bytes(serialization.Encoding.DER).hex()


ADMISSION_MAGIC = b'AMADMIT3'
ADMISSION_INFO = b'Amity admission directional AES-256-GCM v1'


class AdmissionServer:
    """Independent normal-profile server; no setup secret or PAKE fixture.

    The fixture implements only the default button-authorized profile. It
    rejects strict-profile requests instead of selecting a weaker exchange.
    """
    def __init__(self, identity):
        assert re.fullmatch(r'[0-9a-f]{32}', identity) and int(identity, 16)
        self.identity = identity.encode()
        self.phase = 0
        self.tx_sequence = self.rx_sequence = 0
        self.tx = self.rx = None
        self.transcript_hash = None

    @staticmethod
    def packet(kind, body):
        return ADMISSION_MAGIC + bytes([kind, 0]) + body

    def _seal(self, plain):
        assert self.phase != 4 and self.tx and 0 < len(plain) <= 4000 and self.tx_sequence < 2**64-1
        sequence = self.tx_sequence + 1
        header = b'AE3' + bytes([1]) + struct.pack('>Q', sequence)
        result = header + self.tx.encrypt(header, plain, self.transcript_hash + header)
        self.tx_sequence = sequence
        return result

    def _open(self, packet):
        assert self.phase != 4 and self.rx and 29 <= len(packet) <= 4028
        header = packet[:12]
        sequence, = struct.unpack('>Q', header[4:])
        assert header[:4] == b'AE3\x00' and self.rx_sequence < 2**64-1 and sequence == self.rx_sequence + 1
        plain = self.rx.decrypt(header, packet[12:], self.transcript_hash + header)
        self.rx_sequence = sequence
        return plain

    def session(self, packet):
        try:
            if self.phase == 0:
                assert len(packet) == 107 and packet[:10] == ADMISSION_MAGIC + b'\x01\x00'
                public = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), packet[10:75])
                key = ec.generate_private_key(ec.SECP256R1())
                own_public = key.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
                response = self.packet(2, self.identity + own_public + secrets.token_bytes(32))
                self.transcript_hash = hashlib.sha256(packet + response).digest()
                keys = HKDF(algorithm=hashes.SHA256(), length=64, salt=self.transcript_hash, info=ADMISSION_INFO).derive(key.exchange(ec.ECDH(), public))
                self.rx, self.tx = AESGCM(keys[:32]), AESGCM(keys[32:])
                self.phase = 1
                return response
            assert self.phase == 1 and packet[:10] == ADMISSION_MAGIC + b'\x03\x00'
            plain = self._open(packet[10:])
            assert secrets.compare_digest(plain, b'Amity admission client binding v1:' + self.transcript_hash)
            response = self.packet(4, self._seal(b'Amity admission server binding v1:' + self.transcript_hash))
            self.phase = 3
            return response
        except Exception:
            self.phase = 4
            raise

    def crypt(self, packet, encrypt):
        try:
            assert self.phase == 3
            return self._seal(packet) if encrypt else self._open(packet)
        except Exception:
            self.phase = 4
            raise


class Peer:
    def __init__(self, path, result_path, faults=False, legacy_removal=False):
        self.capabilities = KNOWN_CAPABILITIES & (~65536 if legacy_removal else 0xffffffff)
        self.negotiated_capabilities = 0
        self.faults = ['wrong_boot', 'wrong_destination', 'wrong_consortium', 'wrong_config', 'oversized_frame', 'duplicate_json_key'] if faults else []
        self.cert_faults = faults
        self.path = path
        self.result_path = result_path
        if path.exists():
            self.state = json.loads(path.read_text())
        else:
            identity = secrets.token_hex(16)
            key, leaf = new_credential(identity)
            self.state = {'id': identity, 'key': key, 'leaf': leaf}
            self.persist()
        if self.state.pop('password', None) is not None:
            self.persist()  # Retire only this harness's obsolete private Add secret.
        resumed_reset = bool(self.state.get('factory_reset_pending'))
        if resumed_reset:
            self.finish_synthetic_reset()
        self.boot = secrets.token_hex(16)
        self.result = {'peer_id': self.state['id'], 'events': []}
        self.connection_device = None
        self.reset()
        self.operational = None
        if self.state.pop('configuration', None) is not None:
            self.persist()
        self.controller_metadata = None
        self.received_context = None
        self.context_sequence = self.context_generation = 0
        self.control_leases = {}
        self.incoming = self.outgoing = None
        self.last_control = time.monotonic()
        self.link_ready = False
        if resumed_reset:
            self.event('synthetic_factory_reset_recovered', evidence='fixture-private membership/operational credentials cleared after durable marker')

    def persist(self):
        atomic_private(self.path, encoded(self.state))

    def event(self, name, **values):
        self.result['events'].append({'event': name, **values})
        atomic_private(self.result_path, encoded(self.result))
        print(name, flush=True)

    def reset(self):
        self.security = AdmissionServer(self.state['id'])
        self.record = 0
        self.record_endpoint = 0
        self.request = b''
        self.response = b''
        self.response_cursor = 0
        self.record_size = 0
        self.response_read_record = 0
        self.bootstrap_phase = 'session'
        self.last_activity = time.monotonic()

    def bluez_disconnected(self, device):
        # A harness may have unrelated Bluetooth users. Their disconnections
        # must not invalidate this fixture's authenticated GCM counter.
        if self.connection_device is not None and device == self.connection_device:
            self.reset()
            self.connection_device = None
            self.event('gatt_session_reset', reason='own_device_disconnected')
        else:
            self.event('gatt_unrelated_disconnect_ignored')

    def join(self, plain):
        request = json.loads(plain)
        phase = request.get('phase') if isinstance(request, dict) else None
        self.bootstrap_phase = phase if phase in ['identify', 'prepare', 'wifi'] else 'unknown'
        if request == {'phase': 'identify'}:
            self.event('encrypted_identity_requested')
            return encoded({'phase': 'identity', 'bootstrap_profile': 2, 'octopus_id': self.state['id'],
                            'leaf': self.state['leaf'], 'networked': True})
        assert not self.state.get('retired')
        assert set(request) == {'phase', 'proposal', 'digest'} and request['phase'] == 'prepare'
        proposal = json.loads(request['proposal'])
        assert digest(request['proposal'].encode()) == request['digest']
        assert encoded(dict(sorted(proposal.items()))).decode() == request['proposal']
        assert proposal['candidate_id'] == self.state['id'] and proposal['candidate_leaf'] == self.state['leaf']
        assert set(proposal)=={'bootstrap_profile','candidate_hostname','candidate_id','candidate_leaf','candidate_pin','consortium_id','coordinator_id','coordinator_leaf','coordinator_pin','epoch','host','join_id','protocol_max','protocol_min','revision','role'}
        assert proposal['bootstrap_profile']==2 and proposal['protocol_min']==1 and proposal['protocol_max']==1 and proposal['role']=='actuator' and proposal['epoch']=='1'
        assert isinstance(proposal['candidate_hostname'], str) and re.fullmatch(r'[a-z0-9](?:[a-z0-9-]{0,30}[a-z0-9])?', proposal['candidate_hostname'])
        own = x509.load_der_x509_certificate(bytes.fromhex(proposal['candidate_leaf']))
        assert digest(own.public_key().public_bytes(serialization.Encoding.DER,serialization.PublicFormat.SubjectPublicKeyInfo))==proposal['candidate_pin']
        cert = x509.load_der_x509_certificate(bytes.fromhex(proposal['coordinator_leaf']))
        assert digest(cert.public_key().public_bytes(serialization.Encoding.DER,serialization.PublicFormat.SubjectPublicKeyInfo))==proposal['coordinator_pin']
        cert.public_key().verify(cert.signature, cert.tbs_certificate_bytes, ec.ECDSA(hashes.SHA256()))
        assert cert.extensions.get_extension_for_class(x509.SubjectAlternativeName).value.get_values_for_type(x509.DNSName) == ['o-' + proposal['coordinator_id'] + '.amity.invalid']
        prior = self.state.get('proposal')
        assert prior is None or prior == proposal
        self.state['hostname'] = proposal['candidate_hostname']  # This fixture's synthetic identity; never changes Pi network settings.
        self.state['proposal'] = proposal
        self.state['join_digest'] = request['digest']
        self.persist()
        self.event('prepared_after_authenticated_proposal', join_id=proposal['join_id'])
        if self.operational is None or self.operational.done():
            self.operational = asyncio.create_task(self.member_loop())
        return encoded({'phase': 'prepared', 'join_id': proposal['join_id'], 'digest': request['digest'], 'hostname': self.state['hostname']})

    def write(self, data):
        try:
            self._write(data)
        except Exception as error:
            # Never include exception text: parser/crypto failures can contain
            # plaintext, a setup code, or a certificate. Numeric framing state
            # and a fixed phase/category are enough to diagnose interoperability.
            self.event('gatt_write_rejected', phase=self.bootstrap_phase,
                       category=type(error).__name__, record=self.record,
                       encrypted=bool(self.record_endpoint), record_bytes=self.record_size,
                       buffered_bytes=len(self.request), write_bytes=len(data))
            raise

    def _write(self, data):
        if time.monotonic() - self.last_activity >= 30:
            self.reset()
        self.last_activity = time.monotonic()
        if len(data) == 7 and data[0] == 2:
            record, offset = struct.unpack('>IH', data[1:])
            assert record == self.record and offset <= len(self.response)
            self.response_cursor = offset
            return
        assert 10 < len(data) <= 522 and data[0] == 1 and data[1] <= 1
        endpoint = data[1]
        record, total, offset = struct.unpack('>IHH', data[2:10])
        fragment = data[10:]
        assert record and 0 < total <= 4096 and offset + len(fragment) <= total
        if record != self.record:
            assert offset == 0 and record > self.record
            self.record, self.record_endpoint, self.record_size = record, endpoint, total
            self.request, self.response, self.response_cursor = b'', b'', 0
            self.event('gatt_record_started', record=record, encrypted=bool(endpoint), record_bytes=total)
        assert endpoint == self.record_endpoint and total == self.record_size
        if offset < len(self.request):
            assert self.request[offset:offset + len(fragment)] == fragment
            return  # Cached ciphertext; do not consume GCM counter twice.
        assert offset == len(self.request)
        self.request += fragment
        if len(self.request) == total:
            self.event('gatt_record_received', record=record, encrypted=bool(endpoint), record_bytes=total)
            if endpoint:
                self.response = self.security.crypt(self.join(self.security.crypt(self.request, False)), True)
            else:
                self.response = self.security.session(self.request)
                if self.security.phase == 3:
                    self.event('ephemeral_admission_confirmed')
            self.event('gatt_response_ready', record=record, encrypted=bool(endpoint), response_bytes=len(self.response))

    def read(self, mtu):
        header = struct.pack('>BBIHBBH', 1, int(bool(self.response)), self.record, len(self.response), self.record_endpoint, 0, self.response_cursor)
        fragment = self.response[self.response_cursor:self.response_cursor + max(1, min(500, mtu - 13))]
        if self.response and self.response_cursor + len(fragment) == len(self.response) and self.response_read_record != self.record:
            self.response_read_record = self.record
            self.event('gatt_response_read_complete', record=self.record, encrypted=bool(self.record_endpoint), response_bytes=len(self.response), att_mtu=mtu)
        return header + fragment

    def tls_context(self, unknown=False, no_certificate=False):
        proposal = self.state['proposal']
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.minimum_version = ctx.maximum_version = ssl.TLSVersion.TLSv1_2
        ctx.set_ciphers('ECDHE-ECDSA-AES128-GCM-SHA256')
        ctx.set_alpn_protocols(TLS_PROFILES)
        ctx.load_verify_locations(cadata=ssl.DER_cert_to_PEM_cert(bytes.fromhex(proposal['coordinator_leaf'])))
        if not no_certificate:
            key, leaf = (new_credential(secrets.token_hex(16)) if unknown else (self.state['key'], self.state['leaf']))
            keypath, certpath = self.path.with_suffix('.tls-key'), self.path.with_suffix('.tls-cert')
            atomic_private(keypath, key.encode())
            atomic_private(certpath, ssl.DER_cert_to_PEM_cert(bytes.fromhex(leaf)).encode())
            ctx.load_cert_chain(certpath, keypath)
            keypath.unlink()
            certpath.unlink()
        return ctx

    async def connect(self, **options):
        p = self.state['proposal']
        reader, writer = await asyncio.wait_for(asyncio.open_connection(p['host'], 7443, ssl=self.tls_context(**options), server_hostname='o-' + p['coordinator_id'] + '.amity.invalid'), 12)
        assert writer.get_extra_info('ssl_object').getpeercert(binary_form=True).hex() == p['coordinator_leaf']
        return reader, writer

    async def receive(self, reader):
        length, = struct.unpack('>I', await asyncio.wait_for(reader.readexactly(4), 8))
        assert 0 < length <= 8192
        return json.loads(await asyncio.wait_for(reader.readexactly(length), 8))

    async def send(self, writer, value):
        body = encoded(value)
        assert 0 < len(body) <= 8192
        writer.write(struct.pack('>I', len(body)) + body)
        await writer.drain()

    def envelope(self, kind, payload):
        return {'protocol_version': 1, 'consortium_id': self.state['proposal']['consortium_id'],
                'message_id': secrets.token_hex(16), 'origin_octopus_id': self.state['id'],
                'origin_boot_id': self.boot, 'destination_octopus_id': self.state['proposal']['coordinator_id'],
                'destination_boot_id': self.server_boot, 'coordinator_epoch': self.state['proposal']['epoch'],
                'config_revision': self.config_revision, 'kind': kind, 'ttl_ms': 2000, 'payload': payload}

    async def hello(self, reader, writer):
        p = self.state['proposal']
        await self.send(writer, {'type': 'hello', 'protocol_min': 1, 'protocol_max': 1,
             'consortium_id': p['consortium_id'], 'octopus_id': self.state['id'], 'boot_id': self.boot,
             'coordinator_epoch': p['epoch'], 'config_revision': '1', 'config_hash': '0' * 64,
             'firmware_identity': 'independent-python-monitor-peer', 'required_capabilities': REQUIRED_CAPABILITIES,
             'optional_capabilities': self.capabilities & ~REQUIRED_CAPABILITIES, 'frame_cap': 8192,
             **hello_hardware_fields(writer.get_extra_info('ssl_object').selected_alpn_protocol())})
        server = await self.receive(reader)
        assert server['type'] == 'hello' and server['octopus_id'] == p['coordinator_id']
        assert server['consortium_id'] == p['consortium_id'] and server['coordinator_epoch'] == p['epoch']
        assert server['required_capabilities'] & ~self.capabilities == 0
        if writer.get_extra_info('ssl_object').selected_alpn_protocol() == TLS_PROFILES[0]:
            assert 1 <= server['supported_tentacles'] <= 4
            assert 0 <= server['configured_tentacles_mask'] < 1 << server['supported_tentacles']
        self.negotiated_capabilities = (server['required_capabilities'] | server['optional_capabilities']) & self.capabilities
        self.server_boot, self.config_revision = server['boot_id'], server['config_revision']
        link = await self.receive(reader)
        assert set(link) == {'type', 'link_id'} and link['type'] == 'link'
        self.link = link['link_id']
        return await self.receive(reader)

    async def enrollment(self, reader, writer, challenge):
        p = self.state['proposal']
        base = {'join_id': p['join_id'], 'join_digest': self.state['join_digest']}
        challenge = await self.enrollment_controls(reader, writer, challenge)
        assert challenge['kind'] == 'enrollment_control' and challenge['payload']['phase'] == 'challenge'
        await self.send(writer, self.envelope('enrollment_control', {**base, 'phase': 'prove', 'challenge_nonce': challenge['payload']['challenge_nonce']}))
        commit = await self.enrollment_controls(reader, writer, await self.receive(reader))
        assert commit['payload']['phase'] == 'commit' and commit['payload']['membership_revision'] == p['revision']
        self.state['active'] = True
        self.state['membership_revision'] = p['revision']
        self.persist()  # Durable before final acknowledgement.
        await self.send(writer, self.envelope('enrollment_control', {**base, 'phase': 'ack', 'membership_revision': p['revision'], 'state': 'active'}))
        self.event('mutual_tls_lan_commit', cipher=writer.get_extra_info('cipher')[0])
        return await self.receive(reader)

    def scope(self, message):
        expected = self.envelope(message['kind'], message['payload'])
        assert set(message) == set(expected) and message['protocol_version'] == 1
        for key, value in [('consortium_id', expected['consortium_id']), ('origin_octopus_id', expected['destination_octopus_id']),
                ('origin_boot_id', self.server_boot), ('destination_octopus_id', self.state['id']),
                ('destination_boot_id', self.boot), ('coordinator_epoch', expected['coordinator_epoch']),
                ('config_revision', self.config_revision)]:
            assert message[key] == value, 'wrong authenticated ' + key
        assert 1 <= message['ttl_ms'] <= 10000

    async def enrollment_controls(self, reader, writer, message):
        p = self.state['proposal']
        while True:
            self.scope(message)
            payload = message['payload']
            assert message['kind'] == 'enrollment_control' and payload['join_id'] == p['join_id'] and payload['join_digest'] == self.state['join_digest']
            if payload['phase'] not in ['status', 'abort']:
                return message
            state = 'active' if self.state.get('active') else 'prepared'
            if payload['phase'] == 'abort' and not self.state.get('active'):
                self.state['retired'] = True
                self.persist()
                state = 'aborted'
            response = {'phase': 'status', 'join_id': p['join_id'], 'join_digest': self.state['join_digest'], 'request': False, 'state': state}
            if state == 'active':
                response['membership_revision'] = self.state['membership_revision']
            await self.send(writer, self.envelope('enrollment_control', response))
            if state == 'aborted':
                self.event('authenticated_enrollment_aborted')
                raise Retired()
            message = await self.receive(reader)

    def adapter_state(self):
        # An observation-only slot advertises no owned LA, attached device or
        # ability to emit CEC. Link readiness is independent of this report.
        return {'owner': self.state['id'], 'boot': self.boot, 'adapter': 0,
            'front_address': 15, 'back_address': 15, 'front_physical_address': 65535,
            'ready': False, 'discovering': False, 'discovery_generation': 0,
            'setup_generation': 0, 'setup_last_sequence': '0', 'setup_pending': 0, 'setup_replies_pending': False,
            'audio_status_known': False, 'muted': False, 'volume': 127, 'devices': []}

    def owner_state(self):
        assert self.controller_metadata and self.controller_metadata['config_revision'] == self.config_revision
        return encoded({'adapters': [self.adapter_state()]})

    async def send_owner(self, writer):
        if self.outgoing is not None or self.controller_metadata is None or self.controller_metadata['config_revision'] != self.config_revision:
            return
        data = self.owner_state()
        manifest = {'transfer_id': secrets.token_hex(16), 'phase': 'begin', 'owner_octopus_id': self.state['id'],
            'owner_boot_id': self.boot, 'config_revision': self.config_revision, 'total_length': len(data),
            'payload_hash': digest(data), 'document_kind': 'owner_state',
            'chunk_count': (len(data) + 2047) // 2048, 'next_index': 0}
        self.outgoing = {'manifest': manifest, 'data': data, 'index': 0, 'started': time.monotonic()}
        await self.send(writer, self.envelope('snapshot_request', manifest))

    async def handle_snapshot(self, writer, message):
        kind, payload = message['kind'], message['payload']
        if kind == 'snapshot_request' and payload['phase'] != 'begin':
            if self.outgoing is None:
                return
            sent = self.outgoing['manifest']
            for key in ['transfer_id', 'owner_octopus_id', 'owner_boot_id', 'config_revision', 'total_length', 'payload_hash', 'document_kind', 'chunk_count']:
                assert payload[key] == sent[key]
            assert payload['phase'] in ['ack', 'complete'] and payload['next_index'] == self.outgoing['index']
            index = payload['next_index']
            if index == sent['chunk_count']:
                assert payload['phase'] == 'complete'
                self.outgoing = None
                if not self.link_ready:
                    self.link_ready = True
                    self.event('bidirectional_hashed_snapshot_sync', controller_metadata_bytes=self.controller_metadata_bytes, owner_bytes=sent['total_length'])
                return
            assert payload['phase'] == 'ack'
            data = self.outgoing['data'][index * 2048:(index + 1) * 2048]
            await self.send(writer, self.envelope('snapshot_chunk',
                {key: sent[key] for key in ['transfer_id', 'total_length', 'payload_hash']} | {'index': index, 'data': data.hex()}))
            self.outgoing['index'] += 1
            return
        self.last_control = time.monotonic()
        if kind == 'snapshot_request':
            assert payload['phase'] == 'begin' and payload['document_kind'] == 'configuration'
            assert payload['owner_octopus_id'] == self.state['id'] and payload['owner_boot_id'] == self.boot
            assert 0 < payload['total_length'] <= 32768 and payload['chunk_count'] == (payload['total_length'] + 2047) // 2048 <= 16
            assert payload['next_index'] == 0
            self.incoming = {'manifest': payload, 'data': bytearray(), 'index': 0}
            await self.send(writer, self.envelope('snapshot_request', payload | {'phase': 'ack'}))
            return
        assert kind == 'snapshot_chunk' and self.incoming is not None
        incoming = self.incoming
        manifest = incoming['manifest']
        for key in ['transfer_id', 'total_length', 'payload_hash']:
            assert payload[key] == manifest[key]
        assert payload['index'] == incoming['index']
        chunk = bytes.fromhex(payload['data'])
        assert 0 < len(chunk) <= 2048
        incoming['data'].extend(chunk)
        incoming['index'] += 1
        assert len(incoming['data']) <= manifest['total_length']
        complete = incoming['index'] == manifest['chunk_count']
        if complete:
            raw = bytes(incoming['data'])
            assert len(raw) == manifest['total_length'] and digest(raw) == manifest['payload_hash']
            metadata = json.loads(raw)
            assert set(metadata) == {'controller', 'config_revision'}
            assert metadata['controller'] == self.state['proposal']['coordinator_id']
            assert re.fullmatch(r'[1-9][0-9]{0,19}', metadata['config_revision'])
            assert int(metadata['config_revision']) <= 2**64-1
            assert metadata['config_revision'] == manifest['config_revision'] == self.config_revision
            self.controller_metadata_bytes = len(raw)
            self.controller_metadata = metadata  # Link-local identity metadata, never a saved activity replica.
        await self.send(writer, self.envelope('snapshot_request', manifest | {'phase': 'complete' if complete else 'ack', 'next_index': incoming['index']}))
        if complete:
            self.incoming = None
            if not self.link_ready:
                await self.send_owner(writer)

    def finish_synthetic_reset(self):
        # Only this fixture's own private state is reset. No Pi network settings,
        # Bluetooth bonds, adapters, services, or other files are modified.
        receipt = self.state['factory_reset_pending']
        key, leaf = new_credential(self.state['id'])
        self.state = {'id': self.state['id'],
            'key': key, 'leaf': leaf, 'retired': True, 'factory_reset_receipt': receipt}
        self.persist()
        self.controller_metadata = self.incoming = self.outgoing = None
        self.link_ready = False

    async def factory_reset_removal(self, writer, message):
        received = time.monotonic()
        self.scope(message)
        assert message['kind'] == 'membership_event' and self.state.get('active')
        assert self.negotiated_capabilities & 65536
        assert type(message['ttl_ms']) is int and 1 <= message['ttl_ms'] <= 2000
        payload = message['payload']
        assert set(payload) == {'mutation_id', 'proposal_digest', 'phase', 'membership_revision', 'expected_boot_id'}
        assert payload['phase'] == 'factory_reset' and payload['expected_boot_id'] == self.boot
        assert isinstance(payload['mutation_id'], str) and re.fullmatch(r'[0-9a-f]{32}', payload['mutation_id']) and int(payload['mutation_id'], 16)
        assert isinstance(payload['membership_revision'], str) and re.fullmatch(r'[1-9][0-9]{0,19}', payload['membership_revision']) and int(payload['membership_revision']) <= 2**64-1
        proposal = self.state['proposal']
        expected = digest(encoded({'consortium_id': proposal['consortium_id'], 'coordinator_id': proposal['coordinator_id'],
            'coordinator_boot_id': self.server_boot, 'member_id': self.state['id'], 'member_boot_id': self.boot,
            'link_id': self.link, 'epoch': proposal['epoch'], 'config_revision': self.config_revision,
            'membership_revision': payload['membership_revision'], 'mutation_id': payload['mutation_id']}))
        assert payload['proposal_digest'] == expected and int(payload['membership_revision']) > int(self.state.get('membership_revision', proposal['revision']))
        assert not self.state.get('factory_reset_pending') and not self.state.get('retired')
        assert time.monotonic() - received < message['ttl_ms'] / 1000
        self.state['factory_reset_pending'] = dict(payload)
        self.persist()  # Durable authorization precedes ACK, even across a cut.
        acknowledged = False
        try:
            await self.send(writer, self.envelope('membership_result', payload | {'result': 'reset_scheduled'}))
            acknowledged = True
            self.event('authenticated_factory_reset_acknowledged', evidence='fixture durable reset marker and ACK transmitted; DUT receipt not observed')
        finally:
            # Explicit Remove authorized this cleanup even if its ACK is lost.
            self.finish_synthetic_reset()
            self.event('synthetic_factory_reset_completed', acknowledgement_transmitted=acknowledged,
                evidence='fixture-private membership/configuration and old operational leaf cleared; no host reset')
        raise Retired()

    def accept_context(self, payload):
        assert self.link_ready and self.controller_metadata
        assert set(payload) == {'execution_lease_id', 'generation', 'sequence', 'setup_generation', 'target', 'action', 'arguments'}
        assert payload['action'] == 'context'  # No transmitting action is implemented.
        assert self.control_leases.get(payload['execution_lease_id'], 0) > time.monotonic()
        def decimal(value):
            assert isinstance(value, str) and re.fullmatch(r'[1-9][0-9]{0,19}', value) and int(value) <= 2**64-1
            return int(value)
        generation, sequence = decimal(payload['generation']), decimal(payload['sequence'])
        assert generation >= self.context_generation and sequence > self.context_sequence
        assert type(payload['setup_generation']) is int and 0 <= payload['setup_generation'] <= 2**32-1
        def name(value, local=False, allow_empty=False):
            assert isinstance(value, dict) and set(value) == {'consortium_id', 'octopus_id', 'tentacle_id', 'side', 'osd_name'}
            assert value['consortium_id'] == self.state['proposal']['consortium_id']
            assert isinstance(value['octopus_id'], str) and re.fullmatch(r'[0-9a-f]{32}', value['octopus_id']) and int(value['octopus_id'], 16)
            assert type(value['tentacle_id']) is int and 0 <= value['tentacle_id'] < 4
            if local:
                assert value['octopus_id'] == self.state['id'] and value['tentacle_id'] == 0
            assert value['side'] in ['front', 'back']
            osd = value['osd_name']
            assert isinstance(osd, str) and (allow_empty or osd) and len(osd.encode()) <= 14 and all(ord(c) >= 32 and ord(c) != 127 for c in osd)
        name(payload['target'], local=True, allow_empty=True)
        args = payload['arguments']
        assert set(args) == {'selected', 'source_changed', 'source', 'audio', 'display', 'switch_device', 'input'}
        assert type(args['selected']) is bool and type(args['source_changed']) is bool
        assert type(args['input']) is int and 0 <= args['input'] <= 255
        for role in ['source', 'audio', 'display', 'switch_device']:
            if args[role] is not None:
                name(args[role])
        self.received_context = json.loads(encoded(payload))
        self.context_generation, self.context_sequence = generation, sequence

    async def handle(self, writer, message):
        self.scope(message)
        kind = message['kind']
        if kind == 'device_status':
            assert set(message['payload']) == {'hostname'} and re.fullmatch(r'[a-z0-9](?:[a-z0-9-]{0,30}[a-z0-9])?', message['payload']['hostname'])
            await self.send(writer, self.envelope('device_status', {'hostname': self.state['hostname']}))
            self.event('authenticated_hostname_exchanged')
        elif kind in ['snapshot_request', 'snapshot_chunk']:
            await self.handle_snapshot(writer, message)
        elif kind == 'scoped_intent':
            assert type(message['ttl_ms']) is int
            self.accept_context(message['payload'])
        elif kind == 'membership_event':
            await self.factory_reset_removal(writer, message)
        else:
            assert kind == 'heartbeat', 'unimplemented positive authority: ' + kind

    async def rejected_certificates(self):
        for option in ['no_certificate', 'unknown']:
            await self.valid_certificate_control('before_' + option)
            writer = None
            rejected = False
            reason = ''
            try:
                reader, writer = await self.connect(**{option: True})
                await asyncio.wait_for(reader.read(1), 4)
            except ssl.SSLError as exc:
                reason = getattr(exc, 'reason', None) or str(exc)
                alerts = ['SSLV3_ALERT_HANDSHAKE_FAILURE', 'SSLV3_ALERT_BAD_CERTIFICATE',
                          'TLSV1_ALERT_UNKNOWN_CA', 'TLSV13_ALERT_CERTIFICATE_REQUIRED']
                if option == 'unknown':
                    alerts.append('TLSV1_ALERT_ACCESS_DENIED')
                rejected = any(code in reason for code in alerts)
            except (ConnectionError, asyncio.IncompleteReadError, TimeoutError) as exc:
                reason = type(exc).__name__
            finally:
                if writer:
                    writer.close()
                    try:
                        await writer.wait_closed()
                    except Exception:
                        pass
            await asyncio.sleep(0.5)
            await self.valid_certificate_control('after_' + option)
            assert rejected, option + ' has no certificate-alert proof: ' + (reason or 'TLS completed/application EOF')
            self.event('tls_rejected_' + option, reason=reason, valid_certificate_controls=True)

    async def valid_certificate_control(self, label):
        _, writer = await self.connect()
        writer.close()
        await writer.wait_closed()
        self.event('tls_valid_certificate_control_' + label)
        await asyncio.sleep(0.5)

    async def inject_fault(self, reader, writer, fault):
        heartbeat = self.envelope('heartbeat', {'sequence': '1', 'observation_cursor': '0', 'ready': True})
        if fault == 'wrong_boot':
            heartbeat['origin_boot_id'] = secrets.token_hex(16)
        elif fault == 'wrong_destination':
            heartbeat['destination_octopus_id'] = secrets.token_hex(16)
        elif fault == 'wrong_consortium':
            heartbeat['consortium_id'] = secrets.token_hex(16)
        elif fault == 'wrong_config':
            heartbeat['config_revision'] = str(int(self.config_revision) + 1)
        if fault == 'oversized_frame':
            writer.write(struct.pack('>I', 8193))
            await writer.drain()
        elif fault == 'duplicate_json_key':
            body = b'{"protocol_version":1,' + encoded(heartbeat)[1:]
            writer.write(struct.pack('>I', len(body)) + body)
            await writer.drain()
        else:
            await self.send(writer, heartbeat)
        deadline = asyncio.get_running_loop().time() + 5
        while asyncio.get_running_loop().time() < deadline:
            try:
                if not await asyncio.wait_for(reader.read(8192), deadline - asyncio.get_running_loop().time()):
                    self.event('session_rejected_' + fault)
                    return
            except (ssl.SSLError, ConnectionError):
                self.event('session_rejected_' + fault)
                return
        raise AssertionError('invalid session was not closed: ' + fault)

    async def member_loop(self):
        while not self.state.get('retired'):
            writer = read_task = None
            try:
                self.incoming = self.outgoing = self.controller_metadata = self.received_context = None
                self.context_sequence = self.context_generation = 0
                self.control_leases = {}
                self.link_ready = False
                self.last_control = time.monotonic()
                reader, writer = await self.connect()
                begin = await self.hello(reader, writer)
                if begin['kind'] == 'enrollment_control':
                    begin = await self.enrollment(reader, writer, begin)
                await self.handle(writer, begin)
                read_task = asyncio.create_task(self.receive(reader))
                realtime = control = heartbeat = owner = 0
                ready_reported = False
                sequence = 0
                while not self.state.get('retired'):
                    now = time.monotonic()
                    current = self.link_ready and self.controller_metadata['config_revision'] == self.config_revision
                    if current:
                        for kind, duration, due in [('control', 5000, control), ('realtime_input', 500, realtime)]:
                            if now >= due:
                                lease = secrets.token_hex(16)
                                if kind == 'control':
                                    self.control_leases = {key: until for key, until in self.control_leases.items() if until > now}
                                    self.control_leases[lease] = now + duration / 1000
                                await self.send(writer, self.envelope('execution_lease', {'lease_id': lease, 'link_id': self.link, 'class': kind, 'validity_ms': duration}))
                                if kind == 'control':
                                    control = now + 4
                                else:
                                    realtime = now + .25
                        if now >= heartbeat:
                            sequence += 1
                            await self.send(writer, self.envelope('heartbeat', {'sequence': str(sequence), 'observation_cursor': '0', 'ready': True}))
                            heartbeat = now + 1
                        # Synthetic monitor harness traffic only; firmware reports
                        # changed observations and does not use this cadence.
                        if now >= owner and self.outgoing is None:
                            await self.send(writer, self.envelope('scoped_result', {'snapshot': self.adapter_state()}))
                            owner = now + 2
                    if self.outgoing and now - self.outgoing['started'] >= 10:
                        raise TimeoutError('bounded owner snapshot ACK timeout')
                    done, _ = await asyncio.wait({read_task}, timeout=.1)
                    if done:
                        message = read_task.result()
                        await self.handle(writer, message)
                        if message['kind'] == 'heartbeat' and message['payload']['ready'] and self.link_ready and not ready_reported:
                            self.event('operational_ready_and_execution_leases')
                            ready_reported = True
                        read_task = asyncio.create_task(self.receive(reader))
                    quiet = current and ready_reported and self.incoming is None and self.outgoing is None and now - self.last_control >= 5
                    if quiet and self.cert_faults:
                        await self.rejected_certificates()
                        self.cert_faults = False
                        self.last_control = time.monotonic()
                    elif quiet and self.faults:
                        read_task.cancel()
                        await asyncio.gather(read_task, return_exceptions=True)
                        read_task = None
                        await self.inject_fault(reader, writer, self.faults[0])
                        self.faults.pop(0)
                        break
                    elif quiet and not self.result.get('qualified'):
                        self.result['qualified'] = True
                        self.result['scope'] = 'independent passive BLE enrollment and mTLS/session fault fixture only'
                        atomic_private(self.result_path, encoded(self.result))
                        self.event('bounded_peer_checks_complete')
            except Retired:
                break
            except Exception as exc:
                self.event('link_retry', reason=type(exc).__name__ + ': ' + str(exc))
            finally:
                if read_task:
                    read_task.cancel()
                    await asyncio.gather(read_task, return_exceptions=True)
                if writer:
                    writer.close()
                    try:
                        await writer.wait_closed()
                    except Exception:
                        pass
            if not self.state.get('retired'):
                await asyncio.sleep(2)


async def run(args):
    from dbus_next import BusType, Variant
    from dbus_next.aio import MessageBus
    from dbus_next.service import ServiceInterface, method, dbus_property
    from dbus_next.constants import PropertyAccess
    peer = Peer(args.state, args.result, args.faults, args.legacy_removal_capability)
    bus = await MessageBus(bus_type=BusType.SYSTEM).connect()

    class Service(ServiceInterface):
        def __init__(self):
            super().__init__('org.bluez.GattService1')

        @dbus_property(access=PropertyAccess.READ)
        def UUID(self) -> 's':
            return SERVICE

        @dbus_property(access=PropertyAccess.READ)
        def Primary(self) -> 'b':
            return True

        @dbus_property(access=PropertyAccess.READ)
        def Characteristics(self) -> 'ao':
            return [APP + '/service/record']

    class Characteristic(ServiceInterface):
        def __init__(self):
            super().__init__('org.bluez.GattCharacteristic1')

        @dbus_property(access=PropertyAccess.READ)
        def UUID(self) -> 's':
            return RECORD

        @dbus_property(access=PropertyAccess.READ)
        def Service(self) -> 'o':
            return APP + '/service'

        @dbus_property(access=PropertyAccess.READ)
        def Flags(self) -> 'as':
            return ['read', 'write']

        @method()
        def WriteValue(self, value: 'ay', options: 'a{sv}'):
            if 'device' in options:
                peer.connection_device = options['device'].value
            peer.write(bytes(value))

        @method()
        def ReadValue(self, options: 'a{sv}') -> 'ay':
            mtu = options.get('mtu', Variant('q', 23)).value
            return peer.read(mtu)

    class Objects(ServiceInterface):
        def __init__(self):
            super().__init__('org.freedesktop.DBus.ObjectManager')

        @method()
        def GetManagedObjects(self) -> 'a{oa{sa{sv}}}':
            return {APP + '/service': {'org.bluez.GattService1': {'UUID': Variant('s', SERVICE), 'Primary': Variant('b', True), 'Characteristics': Variant('ao', [APP + '/service/record'])}},
                    APP + '/service/record': {'org.bluez.GattCharacteristic1': {'UUID': Variant('s', RECORD), 'Service': Variant('o', APP + '/service'), 'Flags': Variant('as', ['read', 'write'])}}}

    class Advertisement(ServiceInterface):
        def __init__(self):
            super().__init__('org.bluez.LEAdvertisement1')

        @dbus_property(access=PropertyAccess.READ)
        def Type(self) -> 's':
            return 'peripheral'

        @dbus_property(access=PropertyAccess.READ)
        def ServiceUUIDs(self) -> 'as':
            return [SERVICE]

        @dbus_property(access=PropertyAccess.READ)
        def LocalName(self) -> 's':
            return 'A-' + peer.state['id'][-6:]

        @method()
        def Release(self):
            peer.event('advertisement_released')

    def disconnected(message):
        if message.member == 'PropertiesChanged' and message.interface == 'org.freedesktop.DBus.Properties' and len(message.body) >= 2 and message.body[0] == 'org.bluez.Device1':
            connected = message.body[1].get('Connected')
            if connected is not None and not connected.value:
                peer.bluez_disconnected(message.path)
    bus.add_message_handler(disconnected)
    from dbus_next import Message
    await bus.call(Message(destination='org.freedesktop.DBus', path='/org/freedesktop/DBus', interface='org.freedesktop.DBus', member='AddMatch', signature='s', body=["type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'"]))
    bus.export(APP, Objects())
    bus.export(APP + '/service', Service())
    bus.export(APP + '/service/record', Characteristic())
    bus.export(ADV, Advertisement())
    introspection = await bus.introspect('org.bluez', '/org/bluez/hci0')
    adapter = bus.get_proxy_object('org.bluez', '/org/bluez/hci0', introspection)
    gatt = adapter.get_interface('org.bluez.GattManager1')
    advertising = adapter.get_interface('org.bluez.LEAdvertisingManager1')
    await gatt.call_register_application(APP, {})
    registered = False
    try:
        await advertising.call_register_advertisement(ADV, {})
        registered = True
        peer.event('own_gatt_advertisement_registered')
        if peer.state.get('proposal'):
            peer.operational = asyncio.create_task(peer.member_loop())
        await wait_window(args.seconds, args.stop_file)
    finally:
        cleanup_errors = []
        try:
            if registered:
                await advertising.call_unregister_advertisement(ADV)
        except Exception as exc:
            cleanup_errors.append('advertisement:' + type(exc).__name__)
        try:
            await gatt.call_unregister_application(APP)
        except Exception as exc:
            cleanup_errors.append('application:' + type(exc).__name__)
        finally:
            if peer.operational:
                peer.operational.cancel()
                await asyncio.gather(peer.operational, return_exceptions=True)
            bus.disconnect()
        if cleanup_errors:
            raise RuntimeError('Own BlueZ cleanup failed: ' + ','.join(cleanup_errors))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--state', required=True, type=Path)
    parser.add_argument('--result', required=True, type=Path)
    parser.add_argument('--seconds', type=int, default=600)
    parser.add_argument('--stop-file', type=Path, help='Stop when this run-owned marker exists; never changes adapter state')
    parser.add_argument('--legacy-removal-capability', action='store_true', help='Omit reset capability to test update-required removal; no legacy reset handler')
    parser.add_argument('--faults', action='store_true', help='Test certificate rejection and session fences between reconnects')
    args = parser.parse_args()
    if not 1 <= args.seconds <= 1800:
        parser.error('Choose a bounded 1–1800 second reservation')
    asyncio.run(run(args))


if __name__ == '__main__':
    main()
