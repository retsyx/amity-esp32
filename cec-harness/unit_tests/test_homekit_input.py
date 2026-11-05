"""HomeKit scenarios reject changed state and leaked physical control traffic."""

from copy import deepcopy
from pathlib import Path
from types import SimpleNamespace
import sys
import threading
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import Key
from amity_harness import homekit_input as scenario


class HomeKitInputTests(unittest.TestCase):
    def fixture(self):
        state = {"expected_boot_id": "boot", "expected_config_revision": "config",
                 "expected_presentation_revision": "presentation", "current_activity": 1,
                 "activity_settling": False}
        player = SimpleNamespace(gpio=24, address=SimpleNamespace(logical=4))
        session = SimpleNamespace(records=[], _lock=threading.Lock(),
                                  _wire_idle_since_ns={23: 10 ** 30, 24: 10 ** 30},
                                  assert_healthy=Mock())
        scan = SimpleNamespace(_boot="boot", expected=(player,), initiators={23: 3, 24: 3},
                               _session=session, owner_boots={}, owner_duts={}, validate_owners=Mock())
        main = Mock()
        main._runtime = SimpleNamespace(timeout=5, cec_timeout=1)
        main._testbed = SimpleNamespace(control_observations=[])
        main.get_state.return_value = deepcopy(state)
        main.get_configuration.return_value = {"activities": [
            {"activity_id": "other"}, {"activity_id": "selected"}]}

        def wait_for(predicate, *, timeout):
            if not predicate(deepcopy(state)):
                raise TimeoutError("The requested activity did not settle")
            return deepcopy(state)

        main.wait_for.side_effect = wait_for
        activity = SimpleNamespace(activity_id="selected", name="Selected", dut=main, scan=scan,
                                   assert_active=Mock())
        action = SimpleNamespace(dut=main, scan=scan, state=deepcopy(state))
        return main, activity, action, state, scan, player

    def test_settled_requires_same_boot_and_both_saved_revisions(self):
        for field in ("expected_boot_id", "expected_config_revision", "expected_presentation_revision"):
            with self.subTest(field=field):
                main, activity, action, state, scan, _ = self.fixture()
                state[field] = "changed"
                with self.assertRaisesRegex(AssertionError, "boot or saved configuration"):
                    scenario._settled(action, activity)
                activity.assert_active.assert_not_called()

    def test_settled_requires_completed_current_activity(self):
        for change in ({"current_activity": 0}, {"activity_settling": True}):
            with self.subTest(change=change):
                _, activity, action, state, _, _ = self.fixture()
                state.update(change)
                with self.assertRaises(TimeoutError):
                    scenario._settled(action, activity)
                activity.assert_active.assert_not_called()

    def test_select_uses_external_slot_and_checks_array_position_separately(self):
        main, activity, _, _, _, _ = self.fixture()
        homekit = Mock()
        homekit.select_input.return_value = 0
        homekit.read_inputs.return_value = {"current_input": 7}
        # This activity is row 1 in UI state and slot 7 in the HomeKit input list.
        scenario._select(homekit, activity, 7)
        homekit.select_input.assert_called_once_with(7)
        activity.assert_active.assert_called_once_with()
        self.assertTrue(activity._needs_standby)
        self.assertTrue(activity._last_start_terminal)
        main.wait_for.assert_called_once()

    def test_select_rejects_homekit_current_input_mismatch(self):
        _, activity, _, _, _, _ = self.fixture()
        homekit = Mock()
        homekit.select_input.return_value = 0
        homekit.read_inputs.return_value = {"current_input": 1}
        with self.assertRaisesRegex(AssertionError, "ActiveIdentifier"):
            scenario._select(homekit, activity, 7)

    def test_selection_rejection_leaves_fixture_cleanup_armed_without_retry(self):
        main, activity, _, _, _, _ = self.fixture()
        homekit = Mock()
        homekit.select_input.return_value = -70410
        with self.assertRaisesRegex(AssertionError, "rejected a published input"):
            scenario._select(homekit, activity, 7)
        homekit.select_input.assert_called_once_with(7)
        self.assertTrue(activity._needs_standby)
        self.assertFalse(activity._last_start_terminal)
        main.wait_for.assert_not_called()
        homekit.read_inputs.assert_not_called()

    def test_selection_refuses_a_changed_boot_before_sending_any_command(self):
        main, activity, _, _, _, _ = self.fixture()
        main.get_state.return_value["expected_boot_id"] = "changed"
        homekit = Mock()
        with self.assertRaisesRegex(AssertionError, "changed its main boot"):
            scenario._select(homekit, activity, 7)
        homekit.select_input.assert_not_called()

    def test_key_submits_once_and_requires_its_original_single_physical_press(self):
        main, _, _, _, _, player = self.fixture()
        action = SimpleNamespace(started_ns=1_000_000_000, trace={"presses": 1},
                                 assert_received_by=Mock())
        command = Mock(return_value=0)
        with patch.object(scenario, "_prepare_key", return_value=action) as prepare, \
                patch.object(scenario.time, "monotonic_ns", return_value=2_000_000_000):
            self.assertIs(scenario._key(Mock(), main, player, Key.RIGHT, command), action)
        prepare.assert_called_once_with(main, Key.RIGHT, 0)
        command.assert_called_once_with()
        action.assert_received_by.assert_called_once_with(player, timeout=2)
        self.assertEqual(action.kind, "homekit_key")
        self.assertEqual(action.trace["interface"], "homekit")

    def test_failed_key_status_is_not_retried_or_counted_as_physical_success(self):
        for status in (-70410, -70402, False, None):
            with self.subTest(status=status):
                main, _, _, _, _, player = self.fixture()
                action = SimpleNamespace(trace={}, assert_received_by=Mock())
                command = Mock(return_value=status)
                with patch.object(scenario, "_prepare_key", return_value=action):
                    with self.assertRaisesRegex(AssertionError, "rejected a supported accessory command"):
                        scenario._key(Mock(), main, player, Key.RIGHT, command)
                command.assert_called_once_with()
                action.assert_received_by.assert_not_called()

    def test_key_cannot_pass_with_an_acknowledgement_but_missing_wire_evidence(self):
        main, _, _, _, _, player = self.fixture()
        action = SimpleNamespace(started_ns=1_000_000_000, trace={},
                                 assert_received_by=Mock(side_effect=TimeoutError("No physical key")))
        command = Mock(return_value=0)
        with patch.object(scenario, "_prepare_key", return_value=action), \
                patch.object(scenario.time, "monotonic_ns", return_value=2_000_000_000):
            with self.assertRaisesRegex(TimeoutError, "No physical key"):
                scenario._key(Mock(), main, player, Key.RIGHT, command)
        command.assert_called_once_with()

    def test_momentary_key_repetition_is_a_failure(self):
        main, _, _, _, _, player = self.fixture()
        action = SimpleNamespace(started_ns=1_000_000_000, trace={"presses": 2},
                                 assert_received_by=Mock())
        command = Mock(return_value=0)
        with patch.object(scenario, "_prepare_key", return_value=action), \
                patch.object(scenario.time, "monotonic_ns", return_value=2_000_000_000):
            with self.assertRaisesRegex(AssertionError, "single key press"):
                scenario._key(Mock(), main, player, Key.RIGHT, command)
        command.assert_called_once_with()

    def input_fixture(self):
        main, activity, _, _, _, _ = self.fixture()
        other = SimpleNamespace(activity_id="other", name="Other", dut=main)
        activities = (other, activity)
        # Keep both registries out of activity order. Saved slot numbers, not
        # array positions or labels, define the accessory input identities.
        document = {"external_slots": [
            {"slot": 7, "activity_id": "selected", "hidden": False, "tombstone": False},
            {"slot": 2, "activity_id": "other", "hidden": False, "tombstone": False},
            {"slot": 0, "activity_id": "retired", "hidden": False, "tombstone": True}]}
        published = [{"index": 8, "name": "Selected"}, {"index": 3, "name": "Other"}]
        main.request.return_value = document
        homekit = Mock()
        homekit.read_inputs.return_value = {"inputs": published, "current_input": 8}
        return main, homekit, activities, document, published

    def test_input_slots_use_stable_ids_and_preserve_published_evidence(self):
        main, homekit, activities, _, published = self.input_fixture()
        main._testbed.control_observations.append({"kind": "soak", "checks": []})
        self.assertEqual(scenario._input_slots(homekit, main, activities), {"other": 3, "selected": 8})
        main.request.assert_called_once_with("/api/config")
        homekit.read_inputs.assert_called_once_with()
        self.assertEqual(main._testbed.control_observations, [
            {"kind": "soak", "checks": []},
            {"kind": "homekit_inputs", "checks": [{"inputs": published, "passed": True}]}])

    def test_input_slots_reject_missing_hidden_tombstoned_and_ambiguous_assignments(self):
        for change in ("missing", "hidden", "tombstone", "ambiguous"):
            with self.subTest(change=change):
                main, homekit, activities, document, _ = self.input_fixture()
                slot = document["external_slots"][0]
                if change == "missing":
                    document["external_slots"].remove(slot)
                elif change == "ambiguous":
                    document["external_slots"].append({**slot, "slot": 9})
                else:
                    slot[change] = True
                with self.assertRaisesRegex(AssertionError, "unique published accessory slot"):
                    scenario._input_slots(homekit, main, activities)
                homekit.read_inputs.assert_not_called()
                self.assertEqual(main._testbed.control_observations, [])

    def test_input_slots_reject_incomplete_or_mismatched_homekit_publication(self):
        changes = ([{"index": 8, "name": "Selected"}],
                   [{"index": 0, "name": "Other"}, {"index": 1, "name": "Selected"}],
                   [{"index": 2, "name": "Changed"}, {"index": 8, "name": "Selected"}],
                   [{"index": 3, "name": "Other"}, {"index": 8, "name": "Selected"},
                    {"index": 9, "name": "Unexpected"}])
        for published in changes:
            with self.subTest(published=published):
                main, homekit, activities, _, _ = self.input_fixture()
                homekit.read_inputs.return_value["inputs"] = published
                with self.assertRaisesRegex(AssertionError, "input sources"):
                    scenario._input_slots(homekit, main, activities)
                self.assertEqual(main._testbed.control_observations, [])

    def test_standby_requires_accepted_power_off_and_settled_same_owner_state(self):
        main, _, _, state, scan, _ = self.fixture()
        state["current_activity"] = -1
        homekit = Mock()
        homekit.set_power.return_value = 0
        action = scenario._standby(homekit, main, scan)
        homekit.set_power.assert_called_once_with(False)
        self.assertEqual(action.kind, "homekit_power_off")
        self.assertEqual(action.state, state)
        self.assertEqual(action.boot, "boot")
        self.assertIs(action.scan, scan)
        self.assertEqual(scan.validate_owners.call_count, 2)
        main.wait_for.assert_called_once()

    def test_standby_rejection_does_not_wait_retry_or_fall_back(self):
        main, _, _, _, scan, _ = self.fixture()
        homekit = Mock()
        homekit.set_power.return_value = 1
        with self.assertRaisesRegex(AssertionError, "rejected accessory power off"):
            scenario._standby(homekit, main, scan)
        homekit.set_power.assert_called_once_with(False)
        main.wait_for.assert_not_called()
        main.request.assert_not_called()

    def test_standby_rejects_changed_boot_or_configuration(self):
        for field in ("expected_boot_id", "expected_config_revision", "expected_presentation_revision"):
            with self.subTest(field=field):
                main, _, _, state, scan, _ = self.fixture()
                state.update(current_activity=-1, **{field: "changed"})
                homekit = Mock()
                homekit.set_power.return_value = 0
                with self.assertRaisesRegex(AssertionError, "boot or saved configuration"):
                    scenario._standby(homekit, main, scan)
                homekit.set_power.assert_called_once_with(False)

    def test_standby_rejects_an_active_or_in_flight_activity(self):
        for change in ({"current_activity": 1}, {"current_activity": -1, "activity_settling": True}):
            with self.subTest(change=change):
                main, _, _, state, scan, _ = self.fixture()
                state.update(change)
                homekit = Mock()
                homekit.set_power.return_value = 0
                with self.assertRaises(TimeoutError):
                    scenario._standby(homekit, main, scan)

    @staticmethod
    def frame(opcode, *, stamp=150, gpio=24, destination=4, operands=(), **changes):
        raw = bytes([(3 << 4) | destination, opcode, *operands]).hex(":")
        return {"direction": "rx", "transport": "wire", "source": 3, "gpio": gpio,
                "destination": destination, "wire_timestamp_ns": stamp, "opcode": opcode,
                "frame": raw, **changes}

    def rejected(self, records=(), *, recovery=True, statuses=None, input_status=-70410, drained=True,
                 recovery_presses=1):
        main, activity, _, _, scan, player = self.fixture()
        scan._session.records = list(records)
        homekit = Mock()
        homekit.send_key.side_effect = (lambda key: 0 if key == 8 else -70410)
        homekit.select_input.return_value = input_status
        if not drained:
            scan._session._wire_idle_since_ns = {}
        if statuses is not None:
            homekit.send_key.side_effect = statuses

        def receive(client, owner, recipient, key, command):
            self.assertEqual((client, owner, recipient, key), (homekit, main, player, Key.SELECT))
            self.assertEqual(command(), 0)
            if not recovery:
                raise TimeoutError("No physical recovery key")
            return SimpleNamespace(started_ns=200, trace={"presses": recovery_presses,
                                   "press_wire_timestamp_ns": 220, "release_wire_timestamp_ns": 260})

        with patch.object(scenario, "_key", side_effect=receive), \
                patch.object(scenario.time, "monotonic_ns", side_effect=[100, 300]):
            scenario._rejected(homekit, main, scan, activity, player)
        return main, homekit, activity

    def test_rejected_commands_require_supported_key_recovery(self):
        with self.assertRaisesRegex(TimeoutError, "physical recovery key"):
            self.rejected(recovery=False)

    def test_rejected_commands_fail_on_any_leaked_physical_control(self):
        for opcode, operands in ((0x04, ()), (0x0D, ()), (0x36, ()), (0x44, (0x30,)),
                                 (0x45, ()), (0x82, (0x10, 0)), (0x86, (0x10, 0))):
            with self.subTest(opcode=opcode):
                with self.assertRaisesRegex(AssertionError, "physical CEC control"):
                    self.rejected([self.frame(opcode, operands=operands)])

    def test_rejected_commands_fail_on_delayed_or_misrouted_recovery_traffic(self):
        for record in (self.frame(0x44, stamp=250, operands=(0x30,)),
                       self.frame(0x44, stamp=250, operands=(0,), gpio=23),
                       self.frame(0x44, stamp=250, operands=(0,), destination=5),
                       self.frame(0x45, stamp=260, operands=(0,)),
                       self.frame(0x36, stamp=260)):
            with self.subTest(record=record):
                with self.assertRaisesRegex(AssertionError, "physical CEC control"):
                    self.rejected([record])

    def test_rejected_commands_fail_on_extra_select_or_release_to_recovery_recipient(self):
        recovery = [self.frame(0x44, stamp=220, operands=(0,)), self.frame(0x45, stamp=260)]
        for extra in (self.frame(0x44, stamp=230, operands=(0,)), self.frame(0x45, stamp=270)):
            with self.subTest(extra=extra):
                with self.assertRaisesRegex(AssertionError, "physical CEC control"):
                    self.rejected([*recovery, extra])

    def test_recovery_requires_exactly_one_physical_press(self):
        with self.assertRaisesRegex(AssertionError, "single key press"):
            self.rejected(recovery_presses=2)

    def test_rejected_commands_accept_only_recovery_control_on_original_wire(self):
        records = [self.frame(0x44, stamp=220, operands=(0,)), self.frame(0x45, stamp=260),
                   self.frame(0x44, stamp=150, operands=(0x30,), transport="virtual"),
                   self.frame(0x44, stamp=150, operands=(0x30,), direction="tx"),
                   self.frame(0x44, stamp=99, operands=(0x30,))]
        main, homekit, activity = self.rejected(records)
        self.assertEqual(homekit.send_key.call_count, len(scenario.UNSUPPORTED_KEYS) + 1)
        self.assertEqual([call.args[0] for call in homekit.send_key.call_args_list],
                         [12, 13, 14, 16, 255, 8])
        homekit.select_input.assert_called_once_with(17)
        activity.assert_active.assert_called()
        observation = main._testbed.control_observations[-1]
        self.assertEqual(observation["observation_end_monotonic_ns"], 300)
        self.assertEqual(observation["checks"][-2:], [
            {"kind": "invalid_input", "index": 17, "status": -70410, "passed": True},
            {"kind": "recovery", "key": 8, "passed": True}])

    def test_dropped_commands_accept_noop_acknowledgements_and_preserve_actual_statuses(self):
        records = [self.frame(0x44, stamp=220, operands=(0,)), self.frame(0x45, stamp=260)]
        for statuses, input_status in (([0, 0, 0, 0, -70410], 0),
                                       ([-70410, 0, -70410, 0, 0], -70410),
                                       ([0] * 5, 0)):
            with self.subTest(statuses=statuses, input_status=input_status):
                main, homekit, activity = self.rejected(records, statuses=[*statuses, 0], input_status=input_status)
                checks = main._testbed.control_observations[-1]["checks"]
                self.assertEqual(checks[:5], [{"key": key, "status": status}
                                             for key, status in zip(scenario.UNSUPPORTED_KEYS, statuses)])
                self.assertEqual(checks[-2], {"kind": "invalid_input", "index": 17,
                                             "status": input_status, "passed": True})
                self.assertEqual(homekit.send_key.call_count, 6)
                homekit.select_input.assert_called_once_with(17)
                activity.assert_active.assert_called()

    def test_noop_acknowledgements_cannot_hide_a_leaked_physical_command(self):
        for opcode, operands in ((0x44, (12,)), (0x36, ()), (0x82, (0x10, 0))):
            with self.subTest(opcode=opcode):
                with self.assertRaisesRegex(AssertionError, "physical CEC control"):
                    self.rejected([self.frame(opcode, operands=operands)], statuses=[0] * 6, input_status=0)

    def test_unsupported_key_communication_failure_is_not_a_noop_acknowledgement(self):
        for status in (-70402, False, None):
            with self.subTest(status=status):
                with self.assertRaisesRegex(AssertionError, "unexpected status for an unsupported remote key"):
                    self.rejected(statuses=[status])

    def test_unpublished_input_communication_failure_is_not_a_noop_acknowledgement(self):
        for status in (-70402, False, None):
            with self.subTest(status=status):
                with self.assertRaisesRegex(AssertionError, "unexpected status for an unpublished input"):
                    self.rejected(input_status=status)

    def test_rejected_commands_require_wire_observation_through_recovery(self):
        with patch.object(scenario.time, "monotonic", side_effect=[0, 2]):
            with self.assertRaisesRegex(TimeoutError, "observations did not drain"):
                self.rejected(drained=False)


if __name__ == "__main__":
    unittest.main()
