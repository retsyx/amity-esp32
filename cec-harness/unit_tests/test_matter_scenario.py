"""Matter scenarios reject changed state and leaked physical control traffic."""

from copy import deepcopy
from pathlib import Path
from types import SimpleNamespace
import sys
import threading
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import Key
from functional_tests import matter as scenario


class MatterScenarioTests(unittest.TestCase):
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
        matter = Mock()
        matter.select_input.return_value = 0
        matter.read_inputs.return_value = {"current_input": 7}
        # This activity is row 1 in UI state and slot 7 in the Matter input list.
        scenario._select(matter, activity, 7)
        matter.select_input.assert_called_once_with(7)
        activity.assert_active.assert_called_once_with()
        self.assertTrue(activity._needs_standby)
        self.assertTrue(activity._last_start_terminal)
        main.wait_for.assert_called_once()

    def test_select_rejects_matter_current_input_mismatch(self):
        _, activity, _, _, _, _ = self.fixture()
        matter = Mock()
        matter.select_input.return_value = 0
        matter.read_inputs.return_value = {"current_input": 1}
        with self.assertRaisesRegex(AssertionError, "CurrentInput"):
            scenario._select(matter, activity, 7)

    @staticmethod
    def frame(opcode, *, stamp=150, gpio=24, destination=4, operands=(), **changes):
        raw = bytes([(3 << 4) | destination, opcode, *operands]).hex(":")
        return {"direction": "rx", "transport": "wire", "source": 3, "gpio": gpio,
                "destination": destination, "wire_timestamp_ns": stamp, "opcode": opcode,
                "frame": raw, **changes}

    def rejected(self, records=(), *, recovery=True, statuses=None, input_status=1, drained=True,
                 recovery_presses=1):
        main, activity, _, _, scan, player = self.fixture()
        scan._session.records = list(records)
        matter = Mock()
        matter.send_key.side_effect = (lambda key: 0 if key == Key.SELECT else 1)
        matter.select_input.return_value = input_status
        if not drained:
            scan._session._wire_idle_since_ns = {}
        if statuses is not None:
            matter.send_key.side_effect = statuses

        def receive(client, owner, recipient, key, command):
            self.assertEqual((client, owner, recipient, key), (matter, main, player, Key.SELECT))
            self.assertEqual(command(), 0)
            if not recovery:
                raise TimeoutError("No physical recovery key")
            return SimpleNamespace(started_ns=200, trace={"presses": recovery_presses,
                                   "press_wire_timestamp_ns": 220, "release_wire_timestamp_ns": 260})

        with patch.object(scenario, "_key", side_effect=receive), \
                patch.object(scenario.time, "monotonic_ns", side_effect=[100, 300]):
            scenario._rejected(matter, main, scan, activity, player)
        return main, matter, activity

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
        main, matter, activity = self.rejected(records)
        self.assertEqual(matter.send_key.call_count, len(scenario.UNSUPPORTED_KEYS) + 1)
        matter.select_input.assert_called_once_with(255)
        activity.assert_active.assert_called()
        observation = main._testbed.control_observations[-1]
        self.assertEqual(observation["observation_end_monotonic_ns"], 300)
        self.assertEqual(observation["checks"][-2:], [
            {"kind": "invalid_input", "index": 255, "status": 1, "passed": True},
            {"kind": "recovery", "key": int(Key.SELECT), "passed": True}])

    def test_unsupported_key_success_is_a_failure_before_recovery(self):
        with self.assertRaisesRegex(AssertionError, "unsupported Matter keypad"):
            self.rejected(statuses=[0])

    def test_unpublished_input_success_is_a_failure(self):
        with self.assertRaisesRegex(AssertionError, "unpublished input"):
            self.rejected(input_status=0)

    def test_rejected_commands_require_wire_observation_through_recovery(self):
        with patch.object(scenario.time, "monotonic", side_effect=[0, 2]):
            with self.assertRaisesRegex(TimeoutError, "observations did not drain"):
                self.rejected(drained=False)


if __name__ == "__main__":
    unittest.main()
