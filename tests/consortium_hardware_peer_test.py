#!/usr/bin/env python3
"""Local checks for the independent hardware fixture; no hardware claims."""
import hashlib
import asyncio
import copy
import json
import struct
import ssl
import re
import tempfile
import time
from pathlib import Path
import unittest
from unittest.mock import AsyncMock, patch
import consortium_hardware_peer as peer
from consortium_hardware_integration import candidate_token


class NormalClient:
    def __init__(self):
        self.key = peer.ec.generate_private_key(peer.ec.SECP256R1())
        self.tx_sequence = self.rx_sequence = 0
        self.ready = False

    def hello(self):
        public = self.key.public_key().public_bytes(peer.serialization.Encoding.X962, peer.serialization.PublicFormat.UncompressedPoint)
        self.request = peer.ADMISSION_MAGIC + b'\x01\x00' + public + peer.secrets.token_bytes(32)
        return self.request

    def finish(self, welcome):
        assert len(welcome) == 139 and welcome[:10] == peer.ADMISSION_MAGIC + b'\x02\x00'
        public = peer.ec.EllipticCurvePublicKey.from_encoded_point(peer.ec.SECP256R1(), welcome[42:107])
        self.transcript_hash = hashlib.sha256(self.request + welcome).digest()
        keys = peer.HKDF(algorithm=peer.hashes.SHA256(), length=64, salt=self.transcript_hash,
                         info=peer.ADMISSION_INFO).derive(self.key.exchange(peer.ec.ECDH(), public))
        self.tx, self.rx = peer.AESGCM(keys[:32]), peer.AESGCM(keys[32:])
        return peer.ADMISSION_MAGIC + b'\x03\x00' + self.encrypt(b'Amity admission client binding v1:' + self.transcript_hash)

    def confirm(self, response):
        assert response[:10] == peer.ADMISSION_MAGIC + b'\x04\x00'
        assert self.decrypt(response[10:]) == b'Amity admission server binding v1:' + self.transcript_hash
        self.ready = True

    def encrypt(self, plain):
        self.tx_sequence += 1
        header = b'AE3\x00' + struct.pack('>Q', self.tx_sequence)
        return header + self.tx.encrypt(header, plain, self.transcript_hash + header)

    def decrypt(self, packet):
        header = packet[:12]
        sequence, = struct.unpack('>Q', header[4:])
        assert header[:4] == b'AE3\x01' and sequence == self.rx_sequence + 1
        result = self.rx.decrypt(header, packet[12:], self.transcript_hash + header)
        self.rx_sequence = sequence
        return result


class HarnessTests(unittest.TestCase):
    def test_capabilities_are_only_implemented_passive_contracts(self):
        self.assertEqual(peer.KNOWN_CAPABILITIES, 483402)
        self.assertEqual(peer.KNOWN_CAPABILITIES & (1 | 4 | 16 | 32 | 128 | 256 | 512 | 1024 | 2048 | 4096), 0)
    def exchange(self, server=None):
        server = server or peer.AdmissionServer('00000000000000000000000000000001')
        client = NormalClient()
        request = client.hello()
        welcome = server.session(request)
        confirmed = server.session(client.finish(welcome))
        client.confirm(confirmed)
        self.assertEqual((server.phase, client.ready), (3, True))
        self.assertEqual((server.tx_sequence, server.rx_sequence), (1, 1))
        return server, client

    def test_ephemeral_mutual_confirmation_and_directional_gcm(self):
        server, client = self.exchange()
        request = client.encrypt(b'independent request')
        self.assertEqual(server.crypt(request, False), b'independent request')
        reply = server.crypt(b'reply', True)
        self.assertEqual(client.decrypt(reply), b'reply')
        self.assertEqual((server.rx_sequence, server.tx_sequence), (2, 2))
        with self.assertRaises(Exception):
            server.crypt(request, False)
        self.assertEqual(server.phase, 4)  # Replay poisons the whole channel.
        with self.assertRaises(Exception):
            server.crypt(b'later', True)

    def test_wrong_profile_public_and_unconfirmed_app_rejected(self):
        hello = NormalClient().hello()
        for invalid in [hello[:9] + b'\x01' + hello[10:], hello[:10] + bytes(65) + hello[75:],
                        hello + b'extra', b'AMADMIT2' + hello[8:]]:
            server = peer.AdmissionServer('1' * 32)
            with self.assertRaises(Exception):
                server.session(invalid)
            self.assertEqual(server.phase, 4)
        server = peer.AdmissionServer('1' * 32)
        server.session(hello)
        with self.assertRaises(Exception):
            server.crypt(b'unconfirmed', True)

    def test_bad_tag_direction_gap_and_transcript_binding_poison(self):
        for mutate in [lambda p: p[:-1] + bytes([p[-1] ^ 1]),
                       lambda p: p[:3] + b'\x01' + p[4:],
                       lambda p: p[:4] + struct.pack('>Q', 3) + p[12:]]:
            server, client = self.exchange()
            with self.assertRaises(Exception):
                server.crypt(mutate(client.encrypt(b'test')), False)
            self.assertEqual(server.phase, 4)
        server = peer.AdmissionServer('1' * 32)
        client = NormalClient()
        welcome = server.session(client.hello())
        # A substitution changes transcript-derived keys and confirmation.
        changed = welcome[:10] + b'2' * 32 + welcome[42:]
        with self.assertRaises(Exception):
            server.session(client.finish(changed))
        self.assertEqual(server.phase, 4)

    def test_actual_gatt_fragmented_ephemeral_handshake_and_duplicate_response(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = peer.Peer(Path(directory) / 'state', Path(directory) / 'result')
            client = NormalClient()
            def exchange(record, request):
                for offset in range(0, len(request), 10):
                    fixture.write(struct.pack('>BBIHH', 1, 0, record, len(request), offset) + request[offset:offset + 10])
                response = bytearray()
                while len(response) < len(fixture.response):
                    fixture.write(struct.pack('>BIH', 2, record, len(response)))
                    packet = fixture.read(23)
                    self.assertLessEqual(len(packet), 22)
                    response.extend(packet[12:])
                return bytes(response)
            welcome = exchange(1, client.hello())
            finish = client.finish(welcome)
            client.confirm(exchange(2, finish))
            self.assertEqual(fixture.security.phase, 3)
            self.assertEqual((fixture.security.tx_sequence, fixture.security.rx_sequence), (1, 1))
            self.assertIn('ephemeral_admission_confirmed', [event['event'] for event in fixture.result['events']])
            self.assertNotIn('password', json.loads(fixture.path.read_text()))

    def test_integration_selects_exact_scan_and_label_not_another_nearby_board(self):
        scan = {'scan_id': '1' * 32, 'requires_setup_code': False, 'candidates': [
            {'token': 'other', 'label': 'Amity 111111'}, {'token': 'selected', 'label': 'Amity aaaaaa'}]}
        self.assertEqual(candidate_token(scan, '1' * 32, 'Amity aaaaaa'), 'selected')
        self.assertIsNone(candidate_token(scan, '1' * 32, 'Amity bbbbbb'))
        for invalid in [scan | {'scan_id': '2' * 32}, scan | {'requires_setup_code': True},
                        scan | {'candidates': scan['candidates'] + [scan['candidates'][1]]}]:
            with self.assertRaises(AssertionError):
                candidate_token(invalid, '1' * 32, 'Amity aaaaaa')

    def memory_tls(self, replacement=False, client_profiles=None, server_profiles=None, expected_profile=None):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            client_key, client_leaf = peer.new_credential('1' * 32)
            server_key, server_leaf = peer.new_credential('2' * 32)
            server = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            client = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            if client_profiles is not None:
                client.set_alpn_protocols(client_profiles)
            if server_profiles is not None:
                server.set_alpn_protocols(server_profiles)
            for context in [client, server]:
                context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_2
                context.set_ciphers('ECDHE-ECDSA-AES128-GCM-SHA256')
                context.verify_mode = ssl.CERT_REQUIRED
            client.load_verify_locations(cadata=ssl.DER_cert_to_PEM_cert(bytes.fromhex(server_leaf)))
            server.load_verify_locations(cadata=ssl.DER_cert_to_PEM_cert(bytes.fromhex(client_leaf)))
            if replacement:
                client_key, client_leaf = peer.new_credential('1' * 32)
            for name, context, key, leaf in [('client', client, client_key, client_leaf), ('server', server, server_key, server_leaf)]:
                keypath, certpath = base / (name + '.key'), base / (name + '.cert')
                peer.atomic_private(keypath, key.encode())
                peer.atomic_private(certpath, ssl.DER_cert_to_PEM_cert(bytes.fromhex(leaf)).encode())
                context.load_cert_chain(certpath, keypath)
            ci, co, si, so = [ssl.MemoryBIO() for _ in range(4)]
            c = client.wrap_bio(ci, co, server_hostname='o-' + '2' * 32 + '.amity.invalid')
            s = server.wrap_bio(si, so, server_side=True)
            completed = [False, False]
            for _ in range(20):
                for index, context in enumerate([c, s]):
                    try:
                        context.do_handshake()
                        completed[index] = True
                    except ssl.SSLWantReadError:
                        pass
                if co.pending:
                    si.write(co.read())
                if so.pending:
                    ci.write(so.read())
                if all(completed):
                    self.assertEqual(c.getpeercert(binary_form=True).hex(), server_leaf)
                    self.assertEqual(c.cipher()[0], 'ECDHE-ECDSA-AES128-GCM-SHA256')
                    self.assertEqual(c.selected_alpn_protocol(), expected_profile)
                    return
            self.fail('in-memory TLS handshake did not complete')

    def test_offline_exact_leaf_mutual_tls(self):
        self.memory_tls()

    def test_same_identity_replacement_leaf_rejected(self):
        with self.assertRaises(ssl.SSLError):
            self.memory_tls(replacement=True)

    def test_hardware_alpn_and_closed_legacy_extended_fallback(self):
        source = (Path(__file__).resolve().parents[1] / 'components/consortium/tls_link.cpp').read_text()
        profiles = re.findall(r'"([^"]+)"', re.search(r'static const char \*profiles\[\]=\{([^}]+)\}', source)[1])
        self.assertEqual(profiles, peer.TLS_PROFILES)
        self.memory_tls(client_profiles=peer.TLS_PROFILES, server_profiles=profiles, expected_profile=profiles[0])
        self.memory_tls(client_profiles=peer.TLS_PROFILES, server_profiles=profiles[1:], expected_profile=profiles[1])
        self.memory_tls(client_profiles=profiles[1:], server_profiles=profiles, expected_profile=profiles[1])
        self.assertEqual(peer.hello_hardware_fields(profiles[0]), {'supported_tentacles': 1, 'configured_tentacles_mask': 1})
        self.assertEqual(peer.hello_hardware_fields(profiles[1]), {})

    def test_fragment_duplicate_does_not_advance_counter(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = peer.Peer(Path(directory) / 'state', Path(directory) / 'result')
            fixture.security, client = self.exchange()
            fixture.join = lambda plain: b'{"ready":true}'
            ciphertext = client.encrypt(b'{}')
            packet = struct.pack('>BBIHH', 1, 1, 1, len(ciphertext), 0) + ciphertext
            fixture.write(packet)
            self.assertEqual((fixture.security.tx_sequence, fixture.security.rx_sequence), (2, 2))
            response = fixture.read(23)
            self.assertLessEqual(len(response), 22)
            fixture.write(packet)
            self.assertEqual((fixture.security.tx_sequence, fixture.security.rx_sequence), (2, 2))
            self.assertEqual(fixture.read(23), response)
            altered = packet[:-1] + bytes([packet[-1] ^ 1])
            with self.assertRaises(AssertionError):
                fixture.write(altered)
            self.assertEqual((Path(directory) / 'state').stat().st_mode & 0o777, 0o600)


class PassiveSessionTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        base = Path(self.directory.name)
        self.peer = peer.Peer(base / 'state', base / 'result')
        self.peer.state['proposal'] = {'consortium_id': '1' * 32, 'coordinator_id': '2' * 32, 'epoch': '1', 'join_id': '3' * 32, 'revision': '2'}
        self.peer.state['join_digest'] = 'a' * 64
        self.peer.server_boot, self.peer.config_revision, self.peer.link = '4' * 32, '1', '5' * 32
        self.sent = []
        async def capture(writer, message):
            self.sent.append(message)
        self.peer.send = capture
        self.config = {'controller': '2' * 32, 'config_revision': '1'}

    async def transfer(self, config):
        raw = peer.encoded(config)
        manifest = {'transfer_id': '8' * 32, 'phase': 'begin', 'owner_octopus_id': self.peer.state['id'], 'owner_boot_id': self.peer.boot,
            'config_revision': config['config_revision'], 'total_length': len(raw), 'payload_hash': peer.digest(raw), 'document_kind': 'configuration',
            'chunk_count': (len(raw) + 2047) // 2048, 'next_index': 0}
        await self.peer.handle_snapshot(None, {'kind': 'snapshot_request', 'payload': manifest})
        for index in range(manifest['chunk_count']):
            await self.peer.handle_snapshot(None, {'kind': 'snapshot_chunk', 'payload': {k: manifest[k] for k in ['transfer_id', 'total_length', 'payload_hash']} |
                {'index': index, 'data': raw[index * 2048:(index + 1) * 2048].hex()}})
        return manifest

    async def test_initial_owner_state_has_current_required_schema(self):
        await self.transfer(self.config)
        owner = json.loads(self.peer.owner_state())
        self.assertEqual(set(owner), {'adapters'})
        self.assertEqual(owner['adapters'], [self.peer.adapter_state()])
        self.assertEqual(owner['adapters'][0]['devices'], [])
        self.assertFalse(owner['adapters'][0]['ready'])
        self.assertEqual(owner['adapters'][0]['front_address'], 15)
        self.assertEqual(owner['adapters'][0]['setup_last_sequence'], '0')
        self.assertNotIn('configuration', json.loads(self.peer.path.read_text()))
        outgoing = self.peer.outgoing['manifest']
        await self.peer.handle_snapshot(None, {'kind': 'snapshot_request', 'payload': outgoing | {'phase': 'ack'}})
        data = json.loads(bytes.fromhex(self.sent[-1]['payload']['data']))
        self.assertEqual(data, owner)
        await self.peer.handle_snapshot(None, {'kind': 'snapshot_request', 'payload': outgoing | {'phase': 'complete', 'next_index': 1}})
        self.assertTrue(self.peer.link_ready)
        self.assertIsNone(self.peer.outgoing)

    async def test_setup_metadata_is_closed_exact_and_link_local(self):
        original = self.peer.path.read_bytes()
        for changed in [self.config | {'controller': '8' * 32},
                        self.config | {'config_revision': '2'},
                        self.config | {'config_revision': '01'},
                        self.config | {'config_revision': '18446744073709551616'},
                        self.config | {'activities': []}]:
            with self.assertRaises(AssertionError):
                await self.transfer(changed)
            self.assertEqual(self.peer.path.read_bytes(), original)
        await self.transfer(self.config)
        self.assertEqual(self.peer.controller_metadata, self.config)
        self.assertEqual(self.peer.path.read_bytes(), original)
        restarted = peer.Peer(self.peer.path, self.peer.result_path)
        self.assertIsNone(restarted.controller_metadata)

    async def test_authenticated_hostname_metadata_has_no_execution_authority(self):
        self.peer.state['hostname'] = 'bench-member'
        message = self.peer.envelope('device_status', {'hostname': 'living-room'})
        message.update(origin_octopus_id='2' * 32, origin_boot_id=self.peer.server_boot,
                       destination_octopus_id=self.peer.state['id'], destination_boot_id=self.peer.boot)
        await self.peer.handle(None, message)
        self.assertEqual(self.sent[-1]['kind'], 'device_status')
        self.assertEqual(self.sent[-1]['payload'], {'hostname': 'bench-member'})
        self.assertFalse(self.peer.link_ready)
        for bad in ['', 'Wrong Name', 'amity.local', '-amity', 'amity-', 'x' * 33]:
            message['payload'] = {'hostname': bad}
            with self.assertRaises(AssertionError):
                await self.peer.handle(None, message)

    async def test_retired_input_and_management_packets_rejected(self):
        for kind in ['relay_reserve', 'relay_record', 'relay_close', 'relay_result', 'hold_renew', 'ingress_lease', 'intent', 'integration_state', 'remote_management', 'remote_management_result', 'operation']:
            message = self.peer.envelope(kind, {})
            message.update(origin_octopus_id='2' * 32, origin_boot_id=self.peer.server_boot,
                           destination_octopus_id=self.peer.state['id'], destination_boot_id=self.peer.boot)
            with self.assertRaises(AssertionError):
                await self.peer.handle(None, message)
        self.assertFalse(self.sent)

    async def test_monitor_rejects_all_positive_scoped_intents_and_retired_configuration(self):
        for kind in ['scoped_intent', 'config_prepare', 'config_activate', 'config_result']:
            message = self.peer.envelope(kind, {})
            message.update(origin_octopus_id='2' * 32, origin_boot_id=self.peer.server_boot,
                           destination_octopus_id=self.peer.state['id'], destination_boot_id=self.peer.boot)
            with self.assertRaises(AssertionError):
                await self.peer.handle(None, message)
        self.assertFalse(self.sent)
        self.assertNotIn('configuration', json.loads(self.peer.path.read_text()))

    async def test_context_is_nonemitting_exact_scope_and_link_local(self):
        await self.transfer(self.config)
        self.peer.link_ready = True
        self.peer.control_leases['6' * 32] = time.monotonic() + 5
        before = self.peer.path.read_bytes()
        target = {'consortium_id': '1' * 32, 'octopus_id': self.peer.state['id'], 'tentacle_id': 0, 'side': 'front', 'osd_name': ''}
        payload = {'execution_lease_id': '6' * 32, 'generation': '1', 'sequence': '1', 'setup_generation': 0,
            'target': target, 'action': 'context', 'arguments': {'selected': True, 'source_changed': True,
                'source': None, 'audio': None, 'display': None, 'switch_device': None, 'input': 0}}
        message = self.peer.envelope('scoped_intent', payload)
        message.update(origin_octopus_id='2' * 32, origin_boot_id=self.peer.server_boot,
                       destination_octopus_id=self.peer.state['id'], destination_boot_id=self.peer.boot)
        sent = len(self.sent)
        await self.peer.handle(None, message)
        self.assertEqual(self.peer.received_context, payload)
        self.assertEqual(len(self.sent), sent)
        self.assertEqual(self.peer.path.read_bytes(), before)
        for changed in [payload | {'sequence': '1'}, payload | {'sequence': '2', 'action': 'power'},
                        payload | {'sequence': '2', 'execution_lease_id': '7' * 32},
                        payload | {'sequence': '2', 'target': target | {'octopus_id': '8' * 32}},
                        payload | {'sequence': '2', 'arguments': payload['arguments'] | {'extra': True}}]:
            with self.assertRaises(AssertionError):
                await self.peer.handle(None, message | {'payload': changed})
        restarted = peer.Peer(self.peer.path, self.peer.result_path)
        self.assertIsNone(restarted.received_context)
        self.assertEqual(restarted.control_leases, {})

    def removal_message(self):
        self.peer.state['active'] = True
        self.peer.state['membership_revision'] = '2'
        self.peer.negotiated_capabilities = peer.KNOWN_CAPABILITIES
        payload = {'mutation_id': '9' * 32, 'phase': 'factory_reset', 'membership_revision': '3', 'expected_boot_id': self.peer.boot}
        payload['proposal_digest'] = peer.digest(peer.encoded({'consortium_id': '1' * 32, 'coordinator_id': '2' * 32,
            'coordinator_boot_id': self.peer.server_boot, 'member_id': self.peer.state['id'], 'member_boot_id': self.peer.boot,
            'link_id': self.peer.link, 'epoch': '1', 'config_revision': self.peer.config_revision,
            'membership_revision': '3', 'mutation_id': '9' * 32}))
        message = self.peer.envelope('membership_event', payload)
        message['origin_octopus_id'], message['destination_octopus_id'] = message['destination_octopus_id'], message['origin_octopus_id']
        message['origin_boot_id'], message['destination_boot_id'] = message['destination_boot_id'], message['origin_boot_id']
        return message

    async def test_factory_reset_marker_precedes_ack_and_only_fixture_state_is_cleared(self):
        message = self.removal_message()
        original = copy.deepcopy(self.peer.state)
        old_leaf = original['leaf']
        async def capture(writer, response):
            saved = json.loads(self.peer.path.read_text())
            self.assertEqual(saved['factory_reset_pending'], message['payload'])
            self.assertEqual(saved['leaf'], old_leaf)
            self.sent.append(response)
        self.peer.send = capture
        with self.assertRaises(peer.Retired):
            await self.peer.factory_reset_removal(None, message)
        saved = json.loads(self.peer.path.read_text())
        self.assertTrue(saved['retired'])
        self.assertNotIn('proposal', saved)
        self.assertNotIn('active', saved)
        self.assertNotIn('configuration', saved)
        self.assertNotEqual(saved['leaf'], old_leaf)
        self.assertNotEqual(saved['key'], original['key'])
        self.assertEqual(saved['id'], original['id'])
        self.assertNotIn('password', saved)
        self.assertEqual(self.sent[-1]['payload']['result'], 'reset_scheduled')
        self.assertEqual([e['event'] for e in self.peer.result['events']][-2:],
            ['authenticated_factory_reset_acknowledged', 'synthetic_factory_reset_completed'])
        self.assertTrue(self.peer.result['events'][-1]['acknowledgement_transmitted'])

    async def test_factory_reset_lost_ack_still_finishes_authorized_private_cleanup(self):
        message = self.removal_message()
        self.peer.send = AsyncMock(side_effect=ConnectionResetError())
        with self.assertRaises(ConnectionResetError):
            await self.peer.factory_reset_removal(None, message)
        self.assertNotIn('proposal', json.loads(self.peer.path.read_text()))
        self.assertFalse(self.peer.result['events'][-1]['acknowledgement_transmitted'])
        self.assertNotIn('authenticated_factory_reset_acknowledged', [e['event'] for e in self.peer.result['events']])

    async def test_factory_reset_rejects_wrong_scope_digest_boot_revision_ttl_and_legacy(self):
        original = self.removal_message()
        variants = []
        for key in ['origin_boot_id', 'destination_boot_id', 'origin_octopus_id', 'destination_octopus_id', 'consortium_id']:
            bad = copy.deepcopy(original); bad[key] = 'f' * 32; variants.append(bad)
        for key, value in [('config_revision', '2'), ('coordinator_epoch', '2'), ('ttl_ms', 2001), ('ttl_ms', True)]:
            bad = copy.deepcopy(original); bad[key] = value; variants.append(bad)
        for key, value in [('proposal_digest', '0' * 64), ('expected_boot_id', 'f' * 32), ('membership_revision', '2'),
                           ('phase', 'retire'), ('mutation_id', '0' * 32), ('extra', True)]:
            bad = copy.deepcopy(original); bad['payload'][key] = value; variants.append(bad)
        for bad in variants:
            with self.assertRaises(AssertionError):
                await self.peer.factory_reset_removal(None, bad)
            self.assertNotIn('factory_reset_pending', self.peer.state)
        self.peer.state['active'] = False
        with self.assertRaises(AssertionError):
            await self.peer.factory_reset_removal(None, original)
        self.peer.state['active'] = True
        self.peer.negotiated_capabilities &= ~65536
        with self.assertRaises(AssertionError):
            await self.peer.factory_reset_removal(None, original)
        self.assertFalse(self.sent)

    async def test_factory_reset_original_receiver_deadline_is_not_renewed(self):
        message = self.removal_message()
        with patch.object(peer.time, 'monotonic', side_effect=[10, 12]):
            with self.assertRaises(AssertionError):
                await self.peer.factory_reset_removal(None, message)
        self.assertNotIn('factory_reset_pending', self.peer.state)

    async def test_factory_reset_power_cut_marker_resumes_only_own_cleanup(self):
        message = self.removal_message()
        self.peer.state['factory_reset_pending'] = message['payload']
        self.peer.persist()
        previous = copy.deepcopy(self.peer.state)
        resumed = peer.Peer(self.peer.path, self.peer.result_path, legacy_removal=True)
        self.assertEqual(resumed.capabilities, peer.KNOWN_CAPABILITIES & ~65536)
        self.assertNotIn('proposal', resumed.state)
        self.assertNotEqual(previous['leaf'], resumed.state['leaf'])
        self.assertEqual(previous['id'], resumed.state['id'])
        self.assertNotIn('password', resumed.state)
        self.assertEqual(resumed.result['events'][-1]['event'], 'synthetic_factory_reset_recovered')

    async def test_stop_marker_is_bounded_and_has_no_radio_action(self):
        marker = Path(self.directory.name) / 'stop'
        async def stop():
            await asyncio.sleep(.02)
            marker.touch()
        action = asyncio.create_task(stop())
        started = time.monotonic()
        await peer.wait_window(10, marker)
        await action
        self.assertLess(time.monotonic() - started, .8)

    async def test_connection_refused_is_not_certificate_rejection(self):
        self.peer.valid_certificate_control = AsyncMock()
        self.peer.connect = AsyncMock(side_effect=ConnectionRefusedError('test offline'))
        with self.assertRaisesRegex(AssertionError, 'no certificate-alert proof'):
            await self.peer.rejected_certificates()
        self.assertEqual(self.peer.valid_certificate_control.await_count, 2)
        self.assertFalse(any(e['event'].startswith('tls_rejected') for e in self.peer.result['events']))

    async def test_certificate_alert_requires_valid_controls(self):
        self.peer.valid_certificate_control = AsyncMock()
        self.peer.connect = AsyncMock(side_effect=ssl.SSLError(1, 'TLSV1_ALERT_UNKNOWN_CA'))
        await self.peer.rejected_certificates()
        self.assertEqual(self.peer.valid_certificate_control.await_count, 4)
        events = [e for e in self.peer.result['events'] if e['event'].startswith('tls_rejected')]
        self.assertEqual(len(events), 2)
        self.assertTrue(all(e['valid_certificate_controls'] for e in events))

    async def test_unknown_leaf_access_denied_is_specific_certificate_evidence(self):
        self.peer.valid_certificate_control = AsyncMock()
        self.peer.connect = AsyncMock(side_effect=[ssl.SSLError(1, 'SSLV3_ALERT_HANDSHAKE_FAILURE'),
                                                   ssl.SSLError(1, 'TLSV1_ALERT_ACCESS_DENIED')])
        await self.peer.rejected_certificates()
        self.assertEqual(self.peer.valid_certificate_control.await_count, 4)
        self.assertEqual(self.peer.result['events'][-1]['event'], 'tls_rejected_unknown')
        self.assertEqual(self.peer.result['events'][-1]['reason'], 'TLSV1_ALERT_ACCESS_DENIED')

    async def test_reset_timeout_and_plain_eof_are_not_certificate_proof(self):
        for failure in [ConnectionResetError(), TimeoutError(), None]:
            self.peer.valid_certificate_control = AsyncMock()
            writer = AsyncMock()
            writer.close = lambda: None
            reader = AsyncMock(); reader.read.return_value = b''
            self.peer.connect = AsyncMock(side_effect=failure, return_value=(reader, writer))
            with self.assertRaisesRegex(AssertionError, 'no certificate-alert proof'):
                await self.peer.rejected_certificates()
            self.assertEqual(self.peer.valid_certificate_control.await_count, 2)


class AdmissionRecordTests(unittest.IsolatedAsyncioTestCase):
    async def test_encrypted_identify_then_full_prepare_at_default_att_mtu(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            fixture = peer.Peer(base / 'state', base / 'result')
            fixture.security, client = HarnessTests().exchange()
            fixture.member_loop = AsyncMock()  # No network or radio action in this test.

            def exchange(record, plain):
                ciphertext = client.encrypt(plain)
                self.assertLessEqual(len(ciphertext), 4096)
                for offset in range(0, len(ciphertext), 10):
                    packet = struct.pack('>BBIHH', 1, 1, record, len(ciphertext), offset) + ciphertext[offset:offset + 10]
                    fixture.write(packet)
                response = b''
                while len(response) < len(fixture.response):
                    fixture.write(struct.pack('>BIH', 2, record, len(response)))
                    fragment = fixture.read(23)
                    self.assertLessEqual(len(fragment), 22)
                    header = struct.unpack('>BBIHBBH', fragment[:12])
                    self.assertEqual(header, (1, 1, record, len(fixture.response), 1, 0, len(response)))
                    response += fragment[12:]
                return json.loads(client.decrypt(response))

            identity = exchange(3, peer.encoded({'phase': 'identify'}))
            self.assertEqual(identity['leaf'], fixture.state['leaf'])
            coordinator_id = '2' * 32
            _, coordinator_leaf = peer.new_credential(coordinator_id)

            def pin(leaf):
                cert = peer.x509.load_der_x509_certificate(bytes.fromhex(leaf))
                return peer.digest(cert.public_key().public_bytes(peer.serialization.Encoding.DER,
                                  peer.serialization.PublicFormat.SubjectPublicKeyInfo))

            # The exact closed, lexicographic field order emitted by firmware
            # encode_join_proposal; both leaves and SPKI pins are real P-256.
            proposal = {'bootstrap_profile': 2, 'candidate_hostname': 'bench-member', 'candidate_id': fixture.state['id'],
                'candidate_leaf': fixture.state['leaf'], 'candidate_pin': pin(fixture.state['leaf']),
                'consortium_id': '1' * 32, 'coordinator_id': coordinator_id,
                'coordinator_leaf': coordinator_leaf, 'coordinator_pin': pin(coordinator_leaf),
                'epoch': '1', 'host': '10.0.1.1', 'join_id': '3' * 32,
                'protocol_max': 1, 'protocol_min': 1, 'revision': '2', 'role': 'actuator'}
            raw = peer.encoded(proposal)
            request = peer.encoded({'phase': 'prepare', 'proposal': raw.decode(), 'digest': peer.digest(raw)})
            result = exchange(4, request)
            await fixture.operational
            self.assertEqual(result, {'phase': 'prepared', 'join_id': '3' * 32, 'digest': peer.digest(raw), 'hostname': 'bench-member'})
            self.assertEqual(json.loads(fixture.path.read_text())['proposal'], proposal)
            self.assertTrue(any(e['event'] == 'prepared_after_authenticated_proposal' for e in fixture.result['events']))
            self.assertEqual((fixture.security.tx_sequence, fixture.security.rx_sequence), (3, 3))

    async def test_rejection_diagnostics_have_only_framing_and_error_category(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = peer.Peer(Path(directory) / 'state', Path(directory) / 'result')
            fixture.security, _ = HarnessTests().exchange()
            secret = b'NEVER_LOG_THIS_SECRET'
            with self.assertRaises(Exception):
                fixture.write(struct.pack('>BBIHH', 1, 1, 4, len(secret), 0) + secret)
            event = fixture.result['events'][-1]
            self.assertEqual(event['event'], 'gatt_write_rejected')
            self.assertEqual(set(event), {'event', 'phase', 'category', 'record', 'encrypted',
                                         'record_bytes', 'buffered_bytes', 'write_bytes'})
            self.assertNotIn(secret.decode(), fixture.result_path.read_text())

    async def test_unrelated_bluez_device_disconnect_cannot_reset_security(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = peer.Peer(Path(directory) / 'state', Path(directory) / 'result')
            fixture.security, _ = HarnessTests().exchange()
            fixture.connection_device = '/own-device'
            fixture.security.tx_sequence = 7
            fixture.bluez_disconnected('/unrelated-device')
            self.assertEqual((fixture.security.phase, fixture.security.tx_sequence), (3, 7))
            fixture.bluez_disconnected('/own-device')
            self.assertEqual(fixture.security.phase, 0)
            self.assertIsNone(fixture.connection_device)
            self.assertEqual(fixture.result['events'][-1], {'event': 'gatt_session_reset', 'reason': 'own_device_disconnected'})


if __name__ == '__main__':
    unittest.main()
