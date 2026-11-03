"""Host-only firmware upgrade contract checks; no network or hardware access."""

from copy import deepcopy
from dataclasses import replace
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.parse import urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import DUT
from amity_harness.firmware import FirmwareBundle, FirmwareUpgrade


OLD_IMAGE = 'a1' * 32
NEW_IMAGE = 'b2' * 32
MAIN_BOOT, NEW_MAIN_BOOT = '1' * 32, '2' * 32
GROUP = '3' * 32
VERSION = '0.1.133'
SECRET_TOKEN = 'PRIVATE_FIRMWARE_LOGIN_TOKEN'


def image_bytes(version=VERSION, identity=NEW_IMAGE):
    image = bytearray(4096)
    image[0] = 0xe9
    struct.pack_into('<H', image, 12, 9)
    struct.pack_into('<I', image, 32, 0xabcd5432)
    image[48:80] = version.encode().ljust(32, b'\0')
    image[80:112] = b'amity'.ljust(32, b'\0')
    image[176:208] = bytes.fromhex(identity)
    return bytes(image)


def bundle_bytes(*, image=None, manifest_changes=None, canonical=True):
    image = image_bytes() if image is None else image
    manifest = dict(format=1, release_id='test-upgrade', firmware_version=VERSION,
                    image_bytes=str(len(image)), image_sha256=hashlib.sha256(image).hexdigest(),
                    image_elf_sha256=NEW_IMAGE)
    manifest.update(manifest_changes or {})
    raw = json.dumps(manifest, sort_keys=True, separators=(',', ':') if canonical else (', ', ': ')).encode()
    return struct.pack('>16sIIQ', b'AMITY-UPDATE-V1\n', len(raw), 0, len(image)) + raw + image


class FirmwareBundleTests(unittest.TestCase):
    def load(self, data):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'amity-update.bin'
            path.write_bytes(data)
            return FirmwareBundle.load(path)

    def test_exact_bundle_is_bound_to_native_version_hash_and_payload(self):
        data = bundle_bytes()
        bundle = self.load(data)
        self.assertEqual(bundle.data, data)
        self.assertEqual(bundle.version, VERSION)
        self.assertEqual(bundle.image_elf_sha256, NEW_IMAGE)
        self.assertEqual(bundle.image_sha256, hashlib.sha256(image_bytes()).hexdigest())
        self.assertEqual(bundle.image_bytes, len(image_bytes()))
        self.assertNotIn(repr(data), repr(bundle))

    def test_corrupt_or_inconsistent_inputs_are_rejected_before_any_upload(self):
        valid = bundle_bytes()
        cases = [b'not an update', valid[:-1], valid + b'extra',
                 bundle_bytes(manifest_changes={'image_bytes': '1'}),
                 bundle_bytes(manifest_changes={'image_sha256': '0' * 64}),
                 bundle_bytes(image=image_bytes(version='0.1.132')),
                 bundle_bytes(image=image_bytes(identity=OLD_IMAGE)),
                 bundle_bytes(canonical=False)]
        for data in cases:
            with self.subTest(size=len(data)):
                with self.assertRaises(ValueError):
                    self.load(data)

    def test_unbounded_or_unsupported_envelopes_are_rejected(self):
        valid = bytearray(bundle_bytes())
        for manifest_size, signature_size, image_size in ((8193, 0, 4096), (1, 65, 4096), (1, 0, 0x3cd001)):
            data = bytearray(valid)
            struct.pack_into('>IIQ', data, 16, manifest_size, signature_size, image_size)
            with self.subTest(manifest_size=manifest_size, signature_size=signature_size, image_size=image_size):
                with self.assertRaises(ValueError):
                    self.load(bytes(data))

    def test_native_payload_must_leave_room_for_the_partition_release_trailer(self):
        oversized = image_bytes() + b'\xff' * (0x3cd001 - len(image_bytes()))
        with self.assertRaises(ValueError):
            self.load(bundle_bytes(image=oversized))


class Clock:
    def __init__(self):
        self.now = 100.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class UpgradeFixture:
    """Model only shipping endpoint replies and raw upload responses."""

    def __init__(self, *, lost_response=False, rejected=False, early_reboot=False,
                 no_reboot=False, wrong_image=False, unchanged_peer_boot=False,
                 unready_peer=False, mismatched_ready=False, configuration_change=None,
                 settings_change=False, revision_only=False, rollback_peer=False,
                 waiting_old_peer=False, restart_read_503=False, control_unready=False,
                 final_config_503=False, final_settings_503=False):
        self.flags = dict(lost_response=lost_response, rejected=rejected, early_reboot=early_reboot,
                          no_reboot=no_reboot, wrong_image=wrong_image, unchanged_peer_boot=unchanged_peer_boot,
                          unready_peer=unready_peer, mismatched_ready=mismatched_ready,
                          configuration_change=configuration_change, settings_change=settings_change,
                          revision_only=revision_only, rollback_peer=rollback_peer,
                          waiting_old_peer=waiting_old_peer, restart_read_503=restart_read_503,
                          control_unready=control_unready, final_config_503=final_config_503,
                          final_settings_503=final_settings_503)
        self.clock = Clock()
        self.main = DUT('amity-dut')
        self.main._token = SECRET_TOKEN
        self.requests, self.uploads, self.callbacks, self.logins = [], [], [], []
        self.uploaded, self.index = False, 0
        self.old_peer_reads = 2
        self.main._opener = self
        self.main.authenticate = self.authenticate
        self.bed = SimpleNamespace(_entered=True, _closed=False, add_cleanup=self.callbacks.append,
                                   session=SimpleNamespace(assert_healthy=lambda: None))
        self.main._testbed = self.bed
        self.members = []
        for number, name in enumerate(('amity-dut-2', 'amity-dut-3'), 1):
            candidate = DUT(name)
            member = SimpleNamespace(main=self.main, candidate=candidate, name=name,
                device_id=candidate.identity.device_id, boot=MAIN_BOOT, member_boot=str(number+3)*32,
                _initial_system={'boot_id': MAIN_BOOT, 'membership': {'peers': [
                    {'octopus_id': old.device_id, 'boot_id': old.member_boot} for old in self.members]}})
            self.members.append(member)
        self.config = dict(version=3, consortium_id=GROUP, controller_octopus_id=self.main.identity.device_id,
            config_revision='7', presentation_revision='4', primary_zone_id='4'*32,
            octopuses=[{'octopus_id': owner, 'supported_tentacles': 1} for owner in self.ids],
            adapter_settings=[{'octopus_id': owner, 'tentacle_id': 0, 'default_front_physical_address': '1.0.0.0'} for owner in self.ids],
            components=[{'component_id': '5'*32, 'label': 'Saved player', 'kind': 'source',
                'qualified_names': [{'consortium_id': GROUP, 'octopus_id': self.members[0].device_id,
                                    'tentacle_id': 0, 'side': 'back', 'osd_name': 'Player'}]}],
            activities=[{'activity_id': '6'*32, 'zone_id': '4'*32, 'source': '5'*32, 'label': 'Watch player'}],
            activity_order=['6'*32], zones=[{'zone_id': '4'*32, 'activity_order': ['6'*32]}],
            external_slots=[{'slot': 0, 'activity_id': '6'*32, 'tombstone': False, 'hidden': False, 'alias': 'Watch'}])
        self.bundle = FirmwareBundle(bundle_bytes(), VERSION, len(image_bytes()), NEW_IMAGE,
                                     hashlib.sha256(image_bytes()).hexdigest())
        for member in self.members:
            member._baseline = deepcopy(self.config)

    @property
    def ids(self):
        return [self.main.identity.device_id, *[member.device_id for member in self.members]]

    @property
    def rebooted(self):
        return self.uploaded and not self.flags['no_reboot'] and self.index >= (1 if self.flags['early_reboot'] else 4)

    def authenticate(self):
        if not self.main._token:
            self.logins.append('reauthenticated')
            self.main._token = SECRET_TOKEN + '_NEW'

    def health(self):
        return dict(boot_id=NEW_MAIN_BOOT if self.rebooted else MAIN_BOOT, ready=True,
                    version=VERSION if self.rebooted else '0.1.132',
                    image_elf_sha256=('c3'*32 if self.flags['wrong_image'] else NEW_IMAGE) if self.rebooted else OLD_IMAGE)

    def system(self):
        rows = []
        for number, member in enumerate(self.members, 1):
            ready = not (self.rebooted and self.flags['unready_peer'])
            image = NEW_IMAGE if self.rebooted or self.uploaded and self.flags['mismatched_ready'] else OLD_IMAGE
            rows.append(dict(octopus_id=member.device_id, hostname=member.name, enrolled=True,
                boot_id=str(number+6)*32 if self.rebooted and not self.flags['unchanged_peer_boot'] else str(number+3)*32,
                ready=ready, management_ready=not ready, firmware_identity=image))
        config = self.configuration()
        return dict(octopus_id=self.main.identity.device_id, consortium_id=GROUP, mode='coordinator',
            boot_id=self.health()['boot_id'], ready=not (self.rebooted and self.flags['control_unready']), configuration_pending=False,
            applied_config_revision=config['config_revision'], applied_presentation_revision=config['presentation_revision'],
            membership=dict(coordinator=True, authority_ready=True, member=False, peers=rows))

    def configuration(self):
        value = deepcopy(self.config)
        if self.rebooted:
            if self.flags['revision_only']:
                value.update(config_revision='8', presentation_revision='5')
            if self.flags['configuration_change']:
                self.flags['configuration_change'](value)
        return value

    def settings(self, owner):
        if owner == self.main.identity.device_id:
            name, boot = self.main.name, self.health()['boot_id']
        else:
            row = next(row for row in self.system()['membership']['peers'] if row['octopus_id'] == owner)
            name, boot = row['hostname'], row['boot_id']
        address = '2.0.0.0' if self.rebooted and self.flags['settings_change'] else '1.0.0.0'
        return dict(status='ready', error='', settings=dict(boot_id=boot, hostname=name,
            settings_revision='1', hostname_revision='1', adapters=[dict(tentacle_id=0, configured=True,
                                                                        default_front_physical_address=address)]))

    def job(self, phase='distributing'):
        targets = []
        for index, owner in enumerate(self.ids):
            staged = phase == 'activating' or index == 0 or self.index >= 1 and index == 1
            targets.append(dict(octopus_id=owner, phase='staged' if staged else 'uploading',
                                offset=str(self.bundle.image_bytes if staged else 2048), error=''))
        return dict(job_id='9'*32, phase=phase, automatic=False, image_elf_sha256=NEW_IMAGE,
                    image_bytes=str(self.bundle.image_bytes), targets=targets)

    def open(self, request, timeout):
        path = urlsplit(request.full_url).path
        self.requests.append((request.get_method(), path))
        if path == '/api/firmware':
            self.uploads.append(request)
            if self.flags['rejected']:
                raise HTTPError(request.full_url, 409, 'Rejected', {}, io.BytesIO(b'{"error":"PRIVATE_ERROR_BODY"}'))
            self.uploaded = True
            if self.flags['lost_response']:
                raise ConnectionResetError('PRIVATE_TRANSPORT_DETAIL')
            value = {'staged': True}
        else:
            if self.rebooted and request.get_header('Authorization') == 'Bearer ' + SECRET_TOKEN:
                raise HTTPError(request.full_url, 401, 'Restarted', {}, io.BytesIO(b'{}'))
            for flag, matching in (('final_config_503', path == '/api/config'),
                                   ('final_settings_503', path.startswith('/api/devices/') and path.endswith('/settings'))):
                if self.rebooted and self.flags[flag] and matching:
                    self.flags[flag] = False
                    raise HTTPError(request.full_url, 503, 'Temporarily unavailable', {}, io.BytesIO(b'{}'))
            if path == '/api/setup':
                value = self.health()
            elif path == '/api/system':
                if self.rebooted and self.flags['restart_read_503']:
                    self.flags['restart_read_503'] = False
                    raise HTTPError(request.full_url, 503, 'Starting', {}, io.BytesIO(b'{}'))
                value = self.system()
                if self.rebooted and (self.flags['rollback_peer'] or self.flags['waiting_old_peer'] and self.old_peer_reads):
                    peer = value['membership']['peers'][0]
                    peer.update(ready=False, management_ready=True, firmware_identity=OLD_IMAGE)
                    if self.flags['waiting_old_peer']:
                        peer['boot_id'] = self.members[0].member_boot
                        self.old_peer_reads -= 1
            elif path == '/api/config':
                value = self.configuration()
            elif path == '/api/activity-editor/status':
                value = dict(expected_boot_id=self.health()['boot_id'], current_activity=-1, activity_settling=False)
            elif path == '/api/updates/cohort':
                if not self.uploaded or self.index >= 4:
                    value = dict(phase='idle', automatic=False, targets=[])
                else:
                    value = self.job('activating' if self.index >= 2 else 'distributing')
                    self.index += 1
            elif path.startswith('/api/devices/') and path.endswith('/settings'):
                value = self.settings(path.split('/')[3])
            elif path == '/api/activity-editor':
                value = self.configuration()
            else:
                raise AssertionError('Unexpected shipping endpoint: ' + path)
        return io.BytesIO(json.dumps(value).encode())

    def upgrade(self, **kwargs):
        return FirmwareUpgrade(self.main, self.members, self.bundle, timeout=2, poll_interval=.1, **kwargs)

    def run(self, upgrade):
        with patch('amity_harness.firmware.time.monotonic', self.clock.monotonic), \
             patch('amity_harness.firmware.time.sleep', self.clock.sleep):
            return upgrade.run()


class FirmwareUpgradeTests(unittest.TestCase):
    def test_single_authenticated_upload_requires_staging_new_boots_and_exact_image(self):
        fixture = UpgradeFixture()
        upgrade = fixture.upgrade()
        self.assertIs(fixture.run(upgrade), upgrade)
        self.assertEqual(len(fixture.uploads), 1)
        request = fixture.uploads[0]
        self.assertEqual(request.data, fixture.bundle.data)
        self.assertEqual(request.get_header('Authorization'), 'Bearer ' + SECRET_TOKEN)
        self.assertEqual(request.get_header('Content-type'), 'application/octet-stream')
        self.assertEqual(request.get_header('X-amity-confirm-version'), '0.1.132')
        self.assertEqual(upgrade.after_system['boot_id'], NEW_MAIN_BOOT)
        self.assertTrue(fixture.logins, 'new boot must refresh the old authenticated session')
        checks = [row['check'] for row in upgrade.report['checks']]
        self.assertIn('all_targets_healthy', checks)
        encoded = json.dumps(upgrade.report)
        self.assertNotIn(SECRET_TOKEN, encoded)
        self.assertNotIn('PRIVATE_TRANSPORT_DETAIL', encoded)
        self.assertEqual(fixture.bed.control_observations, [upgrade.report])
        with self.assertRaises(RuntimeError):
            fixture.run(upgrade)
        self.assertEqual(len(fixture.uploads), 1)

    def test_lost_upload_response_is_observed_without_replaying_the_mutation(self):
        fixture = UpgradeFixture(lost_response=True)
        upgrade = fixture.upgrade()
        fixture.run(upgrade)
        self.assertEqual(len(fixture.uploads), 1)
        self.assertIn('upload_response_unconfirmed', [row['check'] for row in upgrade.report['checks']])
        self.assertIn('all_targets_healthy', [row['check'] for row in upgrade.report['checks']])
        self.assertNotIn('PRIVATE_TRANSPORT_DETAIL', json.dumps(upgrade.report))

    def test_explicit_http_rejection_never_replays_or_claims_success(self):
        from amity_harness.dut import AmityHTTPError
        fixture = UpgradeFixture(rejected=True)
        upgrade = fixture.upgrade()
        with self.assertRaises(AmityHTTPError):
            fixture.run(upgrade)
        self.assertEqual(len(fixture.uploads), 1)
        self.assertIsNone(upgrade.after_system)
        self.assertNotIn('all_targets_healthy', [row['check'] for row in upgrade.report['checks']])
        self.assertNotIn('PRIVATE_ERROR_BODY', json.dumps(upgrade.report))

    def test_activation_requires_every_exact_target_to_have_a_complete_staged_image(self):
        for problem in ('missing', 'extra', 'duplicate', 'partial', 'uploading', 'skipped', 'wrong_job'):
            with self.subTest(problem=problem):
                fixture = UpgradeFixture()
                upgrade = fixture.upgrade()
                job = fixture.job('activating')
                if problem == 'missing':
                    job['targets'].pop()
                elif problem == 'extra':
                    job['targets'].append(dict(octopus_id='f'*32, phase='staged', offset=str(fixture.bundle.image_bytes)))
                elif problem == 'duplicate':
                    job['targets'].append(deepcopy(job['targets'][0]))
                elif problem == 'partial':
                    job['targets'][1]['offset'] = '2048'
                elif problem == 'uploading':
                    job['targets'][1]['phase'] = 'uploading'
                elif problem == 'skipped':
                    job['targets'][1]['phase'] = 'skipped'
                else:
                    upgrade._observe_job(fixture.job())
                    job['job_id'] = '8'*32
                with self.assertRaises(AssertionError):
                    upgrade._observe_job(job)

    def test_early_reboot_mismatched_ready_and_wrong_native_image_do_not_pass(self):
        for problem in ('early_reboot', 'mismatched_ready', 'wrong_image'):
            with self.subTest(problem=problem):
                fixture = UpgradeFixture(**{problem: True})
                upgrade = fixture.upgrade()
                with self.assertRaises(AssertionError):
                    fixture.run(upgrade)
                self.assertEqual(len(fixture.uploads), 1)
                self.assertIsNone(upgrade.after_system)

    def test_same_boot_or_unready_member_remains_unproven_until_timeout(self):
        for problem in ('no_reboot', 'unchanged_peer_boot', 'unready_peer', 'control_unready'):
            with self.subTest(problem=problem):
                fixture = UpgradeFixture(**{problem: True})
                upgrade = fixture.upgrade()
                with self.assertRaises(TimeoutError):
                    fixture.run(upgrade)
                self.assertEqual(len(fixture.uploads), 1)
                self.assertIsNone(upgrade.after_system)

    def test_rolled_back_secondary_is_not_hidden_by_a_later_automatic_catchup(self):
        fixture = UpgradeFixture(rollback_peer=True)
        with self.assertRaises(AssertionError):
            fixture.run(fixture.upgrade())
        self.assertEqual(len(fixture.uploads), 1)

    def test_old_member_boot_during_activation_and_startup_503_are_retried_without_upload_replay(self):
        for flags in ({'waiting_old_peer': True}, {'restart_read_503': True}):
            with self.subTest(flags=flags):
                fixture = UpgradeFixture(**flags)
                fixture.run(fixture.upgrade())
                self.assertEqual(len(fixture.uploads), 1)

    def test_transient_upload_diagnostics_are_not_treated_as_terminal_or_exposed(self):
        fixture = UpgradeFixture()
        upgrade = fixture.upgrade()
        job = fixture.job()
        job['targets'][1]['error'] = 'PRIVATE_TRANSIENT_DIAGNOSTIC'
        self.assertFalse(upgrade._observe_job(job))
        self.assertTrue(upgrade._observe_job(fixture.job('activating')))
        self.assertNotIn('PRIVATE_TRANSIENT_DIAGNOSTIC', json.dumps(upgrade.report))

    def test_final_configuration_and_settings_transient_503_retry_without_another_upload(self):
        for flag in ('final_config_503', 'final_settings_503'):
            with self.subTest(read=flag):
                fixture = UpgradeFixture(**{flag: True})
                upgrade = fixture.upgrade()
                fixture.run(upgrade)
                self.assertFalse(fixture.flags[flag], 'the final verification read must encounter the injected failure')
                self.assertEqual(len(fixture.uploads), 1)
                self.assertEqual(upgrade.after_system['boot_id'], NEW_MAIN_BOOT)
                self.assertIn('all_targets_healthy', [row['check'] for row in upgrade.report['checks']])
                self.assertLess(fixture.clock.now, 102.0, 'read recovery must retain the original upgrade deadline')

    def test_firmware_cleanup_callback_retries_503_before_refreshing_owned_boot_proofs(self):
        fixture = UpgradeFixture()
        upgrade = fixture.upgrade()
        fixture.run(upgrade)
        fixture.flags['restart_read_503'] = True
        with patch('amity_harness.firmware.time.monotonic', fixture.clock.monotonic), \
             patch('amity_harness.firmware.time.sleep', fixture.clock.sleep):
            upgrade.close()
        self.assertFalse(fixture.flags['restart_read_503'])
        self.assertEqual(len(fixture.uploads), 1)
        self.assertTrue(all(member.boot == NEW_MAIN_BOOT for member in fixture.members))

    def test_saved_configuration_and_adapter_settings_remain_exact(self):
        edits = [lambda value: value['activities'][0].update(source=None),
                 lambda value: value['components'][0].update(label='Edited equipment'),
                 lambda value: value['external_slots'][0].update(hidden=True),
                 lambda value: value['octopuses'].pop(),
                 lambda value: value['adapter_settings'][0].update(default_front_physical_address='2.0.0.0')]
        for edit in edits:
            with self.subTest(edit=edit):
                fixture = UpgradeFixture(configuration_change=edit)
                with self.assertRaisesRegex(AssertionError, 'changed saved'):
                    fixture.run(fixture.upgrade())
        fixture = UpgradeFixture(settings_change=True)
        with self.assertRaisesRegex(AssertionError, 'saved adapter'):
            fixture.run(fixture.upgrade())
        fixture = UpgradeFixture(revision_only=True)
        self.assertIs(fixture.run(fixture.upgrade()).main, fixture.main)

    def test_rebinding_requires_verified_completion_and_preserves_roster_subsets(self):
        fixture = UpgradeFixture()
        upgrade = fixture.upgrade()
        with self.assertRaises(RuntimeError):
            upgrade.rebind_members()
        before = [deepcopy(member._initial_system) for member in fixture.members]
        fixture.run(upgrade)
        upgrade.rebind_members()
        for member, original in zip(fixture.members, before):
            self.assertEqual(member.boot, NEW_MAIN_BOOT)
            expected = next(row for row in upgrade.after_system['membership']['peers'] if row['octopus_id'] == member.device_id)
            self.assertEqual(member.member_boot, expected['boot_id'])
            self.assertEqual(member._baseline, fixture.config)
            self.assertEqual([row['octopus_id'] for row in member._initial_system['membership']['peers']],
                             [row['octopus_id'] for row in original['membership']['peers']])
            self.assertEqual(member._initial_system['boot_id'], NEW_MAIN_BOOT)

    def test_failed_cleanup_of_an_edited_activity_does_not_strand_other_owned_activities(self):
        def edit_first(value):
            value['activities'][0]['label'] = 'Externally edited'
        fixture = UpgradeFixture(configuration_change=edit_first)
        second = deepcopy(fixture.config['activities'][0])
        second['activity_id'] = '7' * 32
        second['label'] = 'Unchanged owned activity'
        fixture.config['activities'].append(second)
        activities = [SimpleNamespace(activity_id=row['activity_id'], _created_row=deepcopy(row), boot=MAIN_BOOT)
                      for row in fixture.config['activities']]
        upgrade = fixture.upgrade(activities=activities)
        with self.assertRaisesRegex(AssertionError, 'changed saved') as primary:
            fixture.run(upgrade)
        with patch('amity_harness.firmware.time.monotonic', fixture.clock.monotonic), \
             patch('amity_harness.firmware.time.sleep', fixture.clock.sleep):
            with self.assertRaisesRegex(AssertionError, 'edited|changed'):
                upgrade.close()
        self.assertEqual(activities[0].boot, MAIN_BOOT, 'cleanup must not adopt the edited activity')
        self.assertEqual(activities[1].boot, NEW_MAIN_BOOT, 'an earlier cleanup error must not strand unchanged owned resources')
        self.assertIn('changed saved', str(primary.exception))
        self.assertFalse(upgrade._verified)
        self.assertNotIn('all_targets_healthy', [row['check'] for row in upgrade.report['checks']])


if __name__ == '__main__':
    unittest.main()
