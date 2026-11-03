"""Firmware scenario ordering and negative wire proofs; mocks only."""
from contextlib import ExitStack
from copy import deepcopy
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from amity_harness import Component, DUT, Key, PowerState, TestBed, Wiring, assign_addresses
from amity_harness.testbed import TestBedCleanupError
from functional_tests import firmware_upgrade as scenario


class ScenarioFixture:
    def __init__(self, *, missing=None, failure_phase=1, cleanup_member=False):
        self.events, self.members, self.activities, self.created = [], [], [], []
        self.components, self.duts = (), ()
        self.epoch, self.current = 0, None
        self.missing, self.failure_phase, self.cleanup_member = missing, failure_phase, cleanup_member
        self.member_rebound = False
        self.boot, self.group = '1' * 32, '3' * 32
        self.config = dict(version=3, config_revision='7', presentation_revision='4', activities=[], components=[], octopuses=[], external_slots=[])
        self.bundle = SimpleNamespace(image_elf_sha256='b2' * 32)
        self.upgrade = None

    def system(self, main):
        return dict(octopus_id=main.identity.device_id, boot_id=self.boot, consortium_id=self.group,
            ready=True, configuration_pending=False, applied_config_revision='7', applied_presentation_revision='4',
            named_devices=[{'octopus_id': main.identity.device_id, 'hostname': main.name}],
            membership=dict(coordinator=True, authority_ready=True, member=False,
                peers=[{'octopus_id': member.device_id} for member in self.members]))

    def request(self, main, path, data=None, **kwargs):
        if path == '/api/config':
            return deepcopy(self.config)
        if path == '/api/setup':
            return {'image_elf_sha256': 'a1' * 32}
        if path.startswith('/api/devices/') and path.endswith('/settings'):
            owner = path.split('/')[3]
            if owner == main.identity.device_id:
                return dict(status='ready', error='', settings=dict(boot_id=self.boot, hostname=main.name,
                    hostname_revision='1', settings_revision='1', adapters=[dict(tentacle_id=0, configured=True, default_front_physical_address='1.0.0.0')]))
            return {'status': 'offline', 'settings': None}
        raise AssertionError('Unexpected endpoint in mock scenario: ' + path)

    def bed(self, *resources, **kwargs):
        fixture = self
        self.duts = tuple(row for row in resources if isinstance(row, DUT))
        self.components = tuple(row for row in resources if isinstance(row, Component))

        class Session:
            def __init__(self, components):
                self.components = components
            def __enter__(self):
                assign_addresses(self.components)
                fixture.events.append(('cec.open',))
                return self
            def assert_healthy(self):
                pass
            def __exit__(self, *args):
                fixture.events.append(('cec.close',))
                for component in self.components:
                    component.address = None
        return TestBed(*resources, session_factory=Session, **kwargs)

    def add(self, main, candidate, **kwargs):
        fixture = self
        class Member:
            _cleanup_priority, _cleanup_retryable = 2, False
            def __init__(self):
                self.main, self.device_id, self.name = main, candidate.identity.device_id, candidate.name
                self.prior_ids = tuple(member.device_id for member in fixture.members)
                self._closed = False
                fixture.members.append(self)
                fixture.config['octopuses'].append({'octopus_id': self.device_id})
                fixture.events.append(('member.add', self.name))
                main._testbed.add_cleanup(self.close)
                self.assert_connected()
            def assert_connected(self):
                if len(fixture.members) != len(self.prior_ids) + 1:
                    raise AssertionError('A single-add proof cannot absorb later siblings')
                fixture.events.append(('member.connected', self.name))
            def close(self):
                fixture.events.append(('member.remove', self.name))
                if fixture.activities:
                    raise AssertionError('Member cleanup overtook activity cleanup')
                fixture.members.remove(self)
                fixture.config['octopuses'] = [row for row in fixture.config['octopuses'] if row['octopus_id'] != self.device_id]
                self._closed = True
                main.membership_operations.append(dict(phase='cleanup', device_id=self.device_id, reset='scheduled', retained_member=False))
                if fixture.cleanup_member and self.name == fixture.duts[-1].name:
                    raise RuntimeError('injected reset receipt failure')
        return Member()

    def scan(self, main):
        fixture = self
        if self.epoch and not self.member_rebound:
            raise AssertionError('Fresh scan must follow member proof rebinding')
        self.events.append(('scan', self.epoch))
        class Scan:
            def assert_contains(self, *components):
                if components != fixture.components:
                    raise AssertionError('Scan lost a board or its equipment')
            assert_contains_only = assert_contains
            def assert_on(self, component, endpoint):
                if component.endpoint != endpoint:
                    raise AssertionError('Scan changed the owner attachment')
        result = Scan()
        result.epoch = self.epoch
        main.last_scan = result
        return result

    def activity(self, main, name, *, display, audio, source):
        fixture, components = self, (display, audio, source)
        class Activity:
            _cleanup_priority, _cleanup_retryable = 0, False
            def __init__(self):
                self.activity_id = f'{len(fixture.created)+1:032x}'
                self.components, self._closed, self.retained_equipment = components, False, []
                fixture.created.append(self)
                fixture.activities.append(self)
                fixture.config['activities'].append({'activity_id': self.activity_id})
                fixture.events.append(('activity.create', self))
                main._testbed._activities.append(self)
                main._testbed.add_cleanup(self.close)
            def start(self):
                fixture.current = self
                for component in components:
                    component._power = PowerState.ON
                audio._active_input = source.address.physical
                frames = list(components)
                if fixture.missing == 'power' and fixture.epoch == fixture.failure_phase:
                    frames.remove(source)
                fixture.events.append(('activity.start', fixture.epoch, self))
                return SimpleNamespace(kind='start', frames=frames, route=not (fixture.missing == 'route' and fixture.epoch == fixture.failure_phase))
            def assert_active(self):
                if fixture.current is not self:
                    raise AssertionError('Saved activity did not remain selected')
            def standby(self):
                fixture.current = None
                for component in components:
                    component._power = PowerState.STANDBY
                frames = [] if fixture.missing == 'standby' and fixture.epoch == fixture.failure_phase else list(components)
                fixture.events.append(('activity.standby', fixture.epoch, self))
                return SimpleNamespace(kind='standby', frames=frames)
            def close(self):
                fixture.events.append(('activity.remove', self))
                fixture.activities.remove(self)
                fixture.config['activities'] = [row for row in fixture.config['activities'] if row['activity_id'] != self.activity_id]
                self._closed = True
                fixture.current = None
        return Activity()

    def wait_power(self, component, power, *, since, command=None):
        if component not in since.frames or component.power != power or (since.kind == 'standby' and command != 'standby'):
            raise AssertionError('Missing fresh physical power command')
        self.events.append(('wire.power', self.epoch, since.kind, component.dut.name))

    def route(self, avr, player, *, since):
        if not since.route or avr.active_input != player.address.physical:
            raise AssertionError('Missing fresh physical routing command')
        self.events.append(('wire.route', self.epoch, avr.dut.name))

    def key(self, main, key):
        fixture, activity = self, self.current
        target = activity.components[1 if key == Key.VOLUME_UP else 2]
        class Press:
            def assert_received_by(self, component):
                if component is not target or fixture.missing == 'key' and fixture.epoch == fixture.failure_phase:
                    raise AssertionError('Missing fresh physical key command')
                fixture.events.append(('wire.key', fixture.epoch, key, component.dut.name))
        return Press()

    def make_upgrade(self, main, members, bundle, *, activities, **kwargs):
        fixture = self
        class Upgrade:
            _cleanup_priority, _cleanup_retryable = -1, False
            def __init__(self):
                self.members, self.activities = tuple(members), tuple(activities)
                self.prior_subsets = [member.prior_ids for member in members]
                main._testbed.add_cleanup(self.close)
            def run(self):
                fixture.events.append(('firmware.run',))
                fixture.epoch, fixture.boot = 1, '2' * 32
            def rebind_members(self):
                fixture.events.append(('members.rebind',))
                fixture.member_rebound = True
                if self.prior_subsets != [member.prior_ids for member in self.members]:
                    raise AssertionError('Upgrade absorbed later siblings into Add baselines')
            def rebind_activities(self, activities, scan):
                if tuple(activities) != self.activities or scan is not main.last_scan or scan.epoch != 1 or not fixture.member_rebound:
                    raise AssertionError('Reboot must refresh proofs for the original activity instances')
                fixture.events.append(('activities.rebind', tuple(activities)))
            def close(self):
                fixture.events.append(('firmware.cleanup',))
        self.upgrade = Upgrade()
        return self.upgrade

    def run(self, *, invalid_bundle=False):
        with ExitStack() as patches:
            patches.enter_context(patch.object(scenario, 'current_config', return_value=SimpleNamespace(firmware=Path('fixture.bin'), timeout=1, firmware_timeout=2)))
            patches.enter_context(patch.object(scenario.FirmwareBundle, 'load', side_effect=ValueError('Invalid bundle') if invalid_bundle else lambda path: self.bundle))
            patches.enter_context(patch.object(scenario, 'TestBed', self.bed))
            patches.enter_context(patch.object(scenario, 'FirmwareUpgrade', self.make_upgrade))
            patches.enter_context(patch.object(scenario, 'event', side_effect=lambda name, **kwargs: self.events.append((name, kwargs.get('phase')))))
            patches.enter_context(patch.object(DUT, 'authenticate', lambda dut: setattr(dut, '_token', 'MOCK_TOKEN')))
            patches.enter_context(patch.object(DUT, 'get_system', lambda dut: self.system(dut)))
            patches.enter_context(patch.object(DUT, 'get_state', lambda dut: dict(expected_boot_id=self.boot, current_activity=-1, activity_settling=False)))
            patches.enter_context(patch.object(DUT, 'request', lambda dut, path, data=None, **kwargs: self.request(dut, path, data, **kwargs)))
            patches.enter_context(patch.object(DUT, 'logout', lambda dut: (self.events.append(('logout',)), setattr(dut, '_token', None))))
            patches.enter_context(patch.object(DUT, 'close', lambda dut: None))
            patches.enter_context(patch.object(DUT, 'add_amity', lambda main, candidate, **kwargs: self.add(main, candidate, **kwargs)))
            patches.enter_context(patch.object(DUT, 'scan_hdmi', lambda dut: self.scan(dut)))
            patches.enter_context(patch.object(DUT, 'create_activity', lambda main, name, **kwargs: self.activity(main, name, **kwargs)))
            patches.enter_context(patch.object(DUT, 'press_key', lambda dut, key: self.key(dut, key)))
            patches.enter_context(patch.object(Component, 'assert_power', lambda component, power: self.assert_power(component, power)))
            patches.enter_context(patch.object(Component, 'wait_for_power', lambda component, power, **kwargs: self.wait_power(component, power, **kwargs)))
            patches.enter_context(patch.object(Component, 'wait_for_input', lambda component, player, **kwargs: self.route(component, player, **kwargs)))
            scenario.test_firmware_upgrade()

    def assert_power(self, component, power):
        if component.power != power:
            raise AssertionError('Unexpected physical component state')


class FirmwareScenarioTests(unittest.TestCase):
    def assert_cleanup(self, fixture):
        self.assertEqual(len([row for row in fixture.events if row[0] == 'activity.remove']), len(fixture.created))
        self.assertEqual(len([row for row in fixture.events if row[0] == 'member.remove']), len(fixture.duts) - 1)
        self.assertFalse(fixture.activities)
        self.assertFalse(fixture.members)
        self.assertIn(('cec.close',), fixture.events)
        self.assertEqual(fixture.events[-1], ('logout',))
        self.assertTrue(all(component.address is None for component in fixture.components))
        names = [row[0] for row in fixture.events]
        if 'firmware.cleanup' in names:
            self.assertLess(names.index('firmware.cleanup'), names.index('activity.remove'))
        self.assertLess(max(index for index, value in enumerate(names) if value == 'activity.remove'), names.index('member.remove'))
        self.assertLess(max(index for index, value in enumerate(names) if value == 'member.remove'), names.index('firmware.cleanup_verified'))
        self.assertLess(names.index('firmware.cleanup_verified'), names.index('cec.close'))

    def test_all_documented_boards_keep_the_same_activities_and_refresh_proofs_in_order(self):
        fixture = ScenarioFixture()
        fixture.run()
        names = set(Wiring.from_markdown().identities)
        self.assertEqual({dut.name for dut in fixture.duts}, names)
        self.assertEqual(len(fixture.components), len(names) * 3)
        self.assertEqual([row[2] for row in fixture.events if row[:2] == ('activity.start', 0)], fixture.created)
        self.assertEqual([row[2] for row in fixture.events if row[:2] == ('activity.start', 1)], fixture.created)
        self.assertEqual([row for row in fixture.events if row[0] == 'scan'], [('scan', 0), ('scan', 1)])
        self.assertLess(fixture.events.index(('members.rebind',)), fixture.events.index(('scan', 1)))
        for epoch in (0, 1):
            self.assertEqual(len([row for row in fixture.events if row[:2] == ('wire.power', epoch)]), len(names) * 6)
            self.assertEqual(len([row for row in fixture.events if row[:2] == ('wire.route', epoch)]), len(names))
            self.assertEqual(len([row for row in fixture.events if row[:2] == ('wire.key', epoch)]), len(names) * 2)
        self.assertEqual(len([row for row in fixture.events if row[0] == 'member.connected']), len(names)-1)
        self.assert_cleanup(fixture)

    def test_missing_fresh_post_upgrade_commands_fail_even_when_component_state_is_correct(self):
        for missing in ('power', 'route', 'key', 'standby'):
            with self.subTest(missing=missing):
                fixture = ScenarioFixture(missing=missing)
                with self.assertRaisesRegex(AssertionError, 'Missing fresh physical'):
                    fixture.run()
                self.assert_cleanup(fixture)

    def test_pre_upgrade_failure_never_uploads_but_still_cleans_every_owned_resource(self):
        fixture = ScenarioFixture(missing='power', failure_phase=0)
        with self.assertRaisesRegex(AssertionError, 'Missing fresh physical'):
            fixture.run()
        self.assertNotIn(('firmware.run',), fixture.events)
        self.assert_cleanup(fixture)

    def test_one_member_cleanup_error_does_not_skip_other_members_or_release(self):
        fixture = ScenarioFixture(cleanup_member=True)
        with self.assertRaises(TestBedCleanupError):
            fixture.run()
        self.assert_cleanup(fixture)

    def test_cleanup_failure_preserves_the_original_post_upgrade_assertion(self):
        fixture = ScenarioFixture(missing='key', cleanup_member=True)
        with self.assertRaises(TestBedCleanupError) as failure:
            fixture.run()
        self.assertIsInstance(failure.exception.__cause__, AssertionError)
        self.assertIn('Missing fresh physical key', str(failure.exception.__cause__))
        self.assert_cleanup(fixture)

    def test_invalid_local_bundle_is_rejected_before_any_bench_resource_is_opened(self):
        fixture = ScenarioFixture()
        with self.assertRaisesRegex(ValueError, 'Invalid bundle'):
            fixture.run(invalid_bundle=True)
        self.assertEqual(fixture.events, [])
        self.assertFalse(fixture.created)
        self.assertFalse(fixture.members)


if __name__ == '__main__':
    unittest.main()
