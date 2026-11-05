"""Interrupted OTA transfers must prove safe abort or a real same-job restart."""

import io
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch
from urllib.parse import urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from test_firmware_upgrade import (
    UpgradeFixture, MAIN_BOOT, NEW_MAIN_BOOT, NEW_IMAGE, OLD_IMAGE, VERSION,
)


INTERRUPTED_MAIN_BOOT = 'c' * 32
INTERRUPTED_MEMBER_BOOT = 'd' * 32


class InterruptedTransferFixture(UpgradeFixture):
    """Only shipping endpoint responses and the authorized USB reset change."""

    def __init__(self, kind, *, fault=None, lost_response=False):
        super().__init__(lost_response=lost_response)
        self.kind, self.fault = kind, fault
        self.usb_calls = []
        self.recovery_reads = 0
        self.main.usb_reboot = lambda: self.usb_reboot(self.main)
        for member in self.members:
            member.candidate.usb_reboot = lambda member=member: self.usb_reboot(member.candidate)

    @property
    def rebooted(self):
        return self.kind == 'secondary' and bool(self.usb_calls) and self.recovery_reads >= 6

    @property
    def interrupted(self):
        return bool(self.usb_calls) and self.fault != 'no_restart'

    def usb_reboot(self, dut):
        self.usb_calls.append(dut)
        return '/dev/mock-' + dut.name

    def health(self):
        value = super().health()
        if self.kind == 'main' and self.interrupted:
            value['boot_id'] = INTERRUPTED_MAIN_BOOT
            if self.fault == 'wrong_image':
                value.update(image_elf_sha256=NEW_IMAGE, version=VERSION)
            elif self.fault == 'wrong_version':
                value['version'] = 'unexpected-version'
        elif self.kind == 'secondary' and self.interrupted and not self.rebooted and self.fault == 'main_reboot':
            value['boot_id'] = INTERRUPTED_MAIN_BOOT
        return value

    def system(self):
        value = super().system()
        if self.uploaded and not self.usb_calls and self.fault == 'preexisting_target_reboot':
            value['membership']['peers'][0]['boot_id'] = 'e' * 32
        if self.interrupted and not self.rebooted:
            peers = value['membership']['peers']
            if self.kind == 'secondary':
                peers[0]['boot_id'] = INTERRUPTED_MEMBER_BOOT
                if self.fault == 'premature_image':
                    peers[0]['firmware_identity'] = NEW_IMAGE
            if self.fault == 'bystander_reboot':
                peers[1]['boot_id'] = 'e' * 32
            if self.fault == 'missing_peer_boot':
                peers[1]['boot_id'] = None
            if self.fault == 'wrong_roster':
                peers.pop()
            if self.fault == 'wrong_group':
                value['consortium_id'] = 'f' * 32
        if self.rebooted and self.fault == 'no_ota_restart':
            value['membership']['peers'][0]['boot_id'] = INTERRUPTED_MEMBER_BOOT
        return value

    def configuration(self):
        value = super().configuration()
        if self.interrupted and self.fault == 'changed_config':
            value['activities'][0]['label'] = 'Changed while interrupted'
        return value

    def settings(self, owner):
        value = super().settings(owner)
        if self.interrupted and self.fault == 'changed_settings':
            value['settings']['adapters'][0]['default_front_physical_address'] = '2.0.0.0'
        return value

    def transfer_job(self):
        if self.kind == 'main' and self.interrupted:
            return dict(phase='idle', automatic=False, targets=[])
        if self.kind == 'secondary' and self.usb_calls:
            self.recovery_reads += 1
            if self.recovery_reads >= 6:
                return dict(phase='idle', automatic=False, targets=[])
            phase = 'activating' if self.recovery_reads >= 4 else 'distributing'
            value = self.job(phase)
            target = value['targets'][1]
            if phase == 'distributing':
                if self.fault == 'no_transfer_restart':
                    target.update(phase='uploading', offset='2048')
                elif self.recovery_reads <= 2:
                    target.update(phase='queued', offset='0')
                else:
                    target.update(phase='uploading', offset='1024')
            if self.fault == 'replaced_job':
                value['job_id'] = '8' * 32
            return value
        value = self.job()
        value['targets'][1].update(phase='uploading', offset='0' if self.fault == 'no_partial' else '1536')
        if self.fault == 'no_partial':
            for target in value['targets'][1:]:
                target.update(phase='queued', offset='0')
        return value

    def local_status(self):
        value = dict(phase='staged_image', expected_elf_sha256=NEW_IMAGE,
                     running_elf_sha256=OLD_IMAGE, current_boot_id=self.health()['boot_id'])
        if self.fault == 'wrong_staged_hash':
            value['expected_elf_sha256'] = 'f' * 64
        if self.fault == 'wrong_running_hash':
            value['running_elf_sha256'] = NEW_IMAGE
        if self.fault == 'wrong_local_boot':
            value['current_boot_id'] = MAIN_BOOT
        if self.fault == 'incomplete_staging':
            value['phase'] = 'interrupted'
        return value

    def open(self, request, timeout):
        path = urlsplit(request.full_url).path
        self.requests.append((request.get_method(), path))
        if path == '/api/firmware':
            self.uploads.append(request)
            self.uploaded = True
            if self.flags['lost_response']:
                raise ConnectionResetError('PRIVATE_UNCERTAIN_UPLOAD_DETAIL')
            value = {'staged': True}
        elif path == '/api/setup':
            value = self.health()
        elif path == '/api/system':
            value = self.system()
        elif path == '/api/config' or path == '/api/activity-editor':
            value = self.configuration()
        elif path == '/api/activity-editor/status':
            value = dict(expected_boot_id=self.health()['boot_id'], current_activity=-1, activity_settling=False)
        elif path == '/api/updates/cohort':
            value = self.transfer_job() if self.uploaded else dict(phase='idle', automatic=False, targets=[])
        elif path == '/api/updates':
            value = self.local_status()
        elif path.startswith('/api/devices/') and path.endswith('/settings'):
            value = self.settings(path.split('/')[3])
        else:
            raise AssertionError('Unexpected shipping endpoint: ' + path)
        return io.BytesIO(json.dumps(value).encode())

    def invoke(self, upgrade):
        with patch('amity_harness.firmware.time.monotonic', self.clock.monotonic), \
             patch('amity_harness.firmware.time.sleep', self.clock.sleep):
            if self.kind == 'main':
                return upgrade.interrupt_main_transfer()
            return upgrade.run(interrupt_secondary=self.members[0])


class FirmwareInterruptionTests(unittest.TestCase):
    def test_main_abort_requires_observed_partial_transfer_new_boot_and_old_firmware(self):
        fixture = InterruptedTransferFixture('main')
        upgrade = fixture.upgrade()
        fixture.invoke(upgrade)
        self.assertEqual(fixture.usb_calls, [fixture.main])
        self.assertEqual(len(fixture.uploads), 1)
        self.assertEqual(upgrade.after_system['boot_id'], INTERRUPTED_MAIN_BOOT)
        self.assertFalse(upgrade._verified, 'a safe interrupted transfer is not an installed upgrade')
        self.assertNotIn('all_targets_healthy', [row['check'] for row in upgrade.report['checks']])
        with self.assertRaises(RuntimeError):
            fixture.invoke(upgrade)
        self.assertEqual(len(fixture.uploads), 1, 'retry requires a separate explicit attempt')

    def test_main_abort_rejects_lost_firmware_configuration_or_exact_staging_evidence(self):
        for fault in ('wrong_image', 'wrong_version', 'bystander_reboot', 'missing_peer_boot', 'wrong_roster', 'wrong_group',
                      'changed_config', 'changed_settings', 'wrong_staged_hash', 'wrong_running_hash',
                      'wrong_local_boot', 'incomplete_staging'):
            with self.subTest(fault=fault):
                fixture = InterruptedTransferFixture('main', fault=fault)
                upgrade = fixture.upgrade()
                with self.assertRaises((AssertionError, TimeoutError)):
                    fixture.invoke(upgrade)
                self.assertEqual(fixture.usb_calls, [fixture.main])
                self.assertEqual(len(fixture.uploads), 1)
                self.assertFalse(upgrade._verified)

    def test_main_does_not_pass_without_partial_progress_or_a_real_restart(self):
        for fault in ('no_partial', 'no_restart'):
            with self.subTest(fault=fault):
                fixture = InterruptedTransferFixture('main', fault=fault)
                upgrade = fixture.upgrade()
                with self.assertRaises((AssertionError, TimeoutError)):
                    fixture.invoke(upgrade)
                self.assertEqual(len(fixture.usb_calls), 0 if fault == 'no_partial' else 1)
                self.assertEqual(len(fixture.uploads), 1)
                self.assertIsNone(upgrade.after_system)

    def test_secondary_restart_reuses_job_then_requires_another_boot_into_exact_image(self):
        fixture = InterruptedTransferFixture('secondary')
        upgrade = fixture.upgrade()
        self.assertIs(fixture.invoke(upgrade), upgrade)
        self.assertEqual(fixture.usb_calls, [fixture.members[0].candidate])
        self.assertEqual(len(fixture.uploads), 1)
        self.assertTrue(upgrade._verified)
        self.assertEqual(upgrade.after_system['boot_id'], NEW_MAIN_BOOT)
        peer = upgrade.after_system['membership']['peers'][0]
        self.assertNotEqual(peer['boot_id'], INTERRUPTED_MEMBER_BOOT)
        self.assertEqual(peer['firmware_identity'], NEW_IMAGE)
        self.assertIn('all_targets_healthy', [row['check'] for row in upgrade.report['checks']])
        self.assertEqual({request.get_method() for request in fixture.uploads}, {'POST'})

    def test_secondary_faults_cannot_be_hidden_by_eventual_successful_install(self):
        for fault in ('no_restart', 'no_transfer_restart', 'replaced_job', 'bystander_reboot', 'main_reboot',
                      'premature_image', 'wrong_roster', 'wrong_group', 'no_ota_restart'):
            with self.subTest(fault=fault):
                fixture = InterruptedTransferFixture('secondary', fault=fault)
                upgrade = fixture.upgrade()
                with self.assertRaises((AssertionError, TimeoutError)):
                    fixture.invoke(upgrade)
                self.assertEqual(fixture.usb_calls, [fixture.members[0].candidate])
                self.assertEqual(len(fixture.uploads), 1)
                self.assertFalse(upgrade._verified)

    def test_secondary_interruption_is_required_even_if_transfer_never_becomes_partial(self):
        fixture = InterruptedTransferFixture('secondary', fault='no_partial')
        upgrade = fixture.upgrade()
        with self.assertRaises((AssertionError, TimeoutError)):
            fixture.invoke(upgrade)
        self.assertFalse(fixture.usb_calls)
        self.assertEqual(len(fixture.uploads), 1)
        self.assertFalse(upgrade._verified)

    def test_unexpected_target_boot_before_requested_usb_reboot_is_not_adopted(self):
        fixture = InterruptedTransferFixture('secondary', fault='preexisting_target_reboot')
        upgrade = fixture.upgrade()
        with self.assertRaises(AssertionError):
            fixture.invoke(upgrade)
        self.assertFalse(fixture.usb_calls, 'reject an already changed boot before issuing the requested reset')
        self.assertEqual(len(fixture.uploads), 1)
        self.assertFalse(upgrade._verified)

    def test_uncertain_response_does_not_replay_either_interrupted_upload(self):
        for kind in ('main', 'secondary'):
            with self.subTest(kind=kind):
                fixture = InterruptedTransferFixture(kind, lost_response=True)
                upgrade = fixture.upgrade()
                fixture.invoke(upgrade)
                self.assertEqual(len(fixture.uploads), 1)
                self.assertEqual(len(fixture.usb_calls), 1)
                self.assertNotIn('PRIVATE_UNCERTAIN_UPLOAD_DETAIL', json.dumps(upgrade.report))


if __name__ == '__main__':
    unittest.main()
