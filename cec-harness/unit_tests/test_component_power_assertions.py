"""Power assertions require actual handled DUT commands, not UI/model labels."""

from contextlib import contextmanager
from copy import deepcopy
from dataclasses import replace
from pathlib import Path
import sys
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import (AvrComponent, CecError, CecSession, DUT, Frame, Hooks,
                           PlaybackComponent, PowerState, RunConfig, TvComponent,
                           TxResult, use_config)
from test_activity import Fixture as ActivityFixture


def distributed_activity_fixture():
    """Current coordinator/member evidence with direct hardware-free RX."""
    fixture = ActivityFixture()
    fixture.add_member()
    fixture.session._lock = threading.RLock()
    fixture.session._stop = threading.Event()
    fixture.session._record = lambda *args, **kwargs: None
    fixture.session.power_changed = lambda *args, **kwargs: None
    return fixture


class IdleWire:
    def __init__(self, gpio):
        self.gpio = gpio
        self.idle = threading.Event()

    def open(self):
        pass

    def configure(self, components):
        pass

    def transmit(self, frame):
        return TxResult(frame, 1, time.monotonic_ns())

    def receive(self, timeout):
        self.idle.wait(timeout)
        return None

    def close(self):
        self.idle.set()


class ComponentPowerAssertionTests(unittest.TestCase):
    @contextmanager
    def fixture(self):
        dut = DUT("amity-dut")
        tentacle = dut.tentacle(0)
        tv = TvComponent(tentacle.front, "Power TV")
        avr = AvrComponent(tentacle.back, "Power AVR")
        player = PlaybackComponent(avr, "Power Player", port=1)
        with CecSession([tv, avr, player], backend_factory=IdleWire) as session:
            dut.last_scan = SimpleNamespace(initiators={tv.gpio: 4, avr.gpio: 0}, _session=session)
            yield dut, tv, avr, player, session

    def boundary(self, dut, session, *, started=None):
        return SimpleNamespace(dut=dut, session=session,
                               started_ns=time.monotonic_ns() if started is None else started,
                               initiators=dict(dut.last_scan.initiators))

    def receive(self, component, source, opcode, operands=b"", *, timestamp=None, transport="wire"):
        component.receive(Frame(source, component.address.logical, opcode, operands,
                                timestamp_ns=time.monotonic_ns() if timestamp is None else timestamp),
                          transport=transport)

    def missing(self, component, state, boundary):
        with self.assertRaises(TimeoutError):
            component.wait_for_power(state, since=boundary, timeout=0.005)

    def test_main_action_authorizes_only_proven_member_wire_power_and_checkpoint(self):
        f = distributed_activity_fixture()
        activity = f.activity(display=f.member_tv, source=f.member_player, audio=f.member_avr)
        action = activity.start()
        self.receive(f.member_tv, 4, 0x04)
        self.assertIs(f.member_tv.wait_for_power("on", since=action), f.member_tv)
        marker = f.member_tv.checkpoint()
        self.receive(f.member_tv, 4, 0x36)
        self.assertIs(f.member_tv.wait_for_power("standby", since=marker, command="standby"), f.member_tv)

    def test_member_wire_assertions_reject_foreign_action_owner_boot_maps_and_session(self):
        f = distributed_activity_fixture()
        action = f.activity(display=f.member_tv, source=f.member_player, audio=f.member_avr).start()
        self.receive(f.member_tv, 4, 0x04)
        wrong_boots = {**action.owner_boots, f.member.identity.device_id: "e" * 32}
        wrong_owners = {**action.owner_duts, f.member.identity.device_id: DUT("amity-dut-2")}
        for changed in (dict(dut=f.member), dict(session=object()), dict(testbed=object()), dict(scan=object()),
                        dict(boot="e" * 32), dict(owner_boots=wrong_boots), dict(owner_duts=wrong_owners),
                        dict(initiators={**action.initiators, f.member_tv.gpio: 8})):
            with self.subTest(changed=tuple(changed)), self.assertRaises(AssertionError):
                f.member_tv.wait_for_power("on", since=replace(action, **changed), timeout=0.01)
        f.bed._closed = True
        with self.assertRaises(AssertionError):
            f.member_tv.wait_for_power("on", since=action, timeout=0.01)

    def test_virtual_or_stale_member_power_cannot_satisfy_main_action(self):
        f = distributed_activity_fixture()
        old = time.monotonic_ns()
        action = f.activity(display=f.member_tv, source=f.member_player, audio=f.member_avr).start()
        self.receive(f.member_tv, 4, 0x04, transport="virtual")
        self.missing(f.member_tv, "on", action)
        f.member_tv.hooks.before_rx = lambda component, frame: Frame(
            frame.source, frame.destination, frame.opcode, timestamp_ns=time.monotonic_ns())
        self.receive(f.member_tv, 4, 0x04, timestamp=old)
        self.missing(f.member_tv, "on", action)

    def test_current_member_and_main_boots_are_rechecked_before_accepting_power(self):
        for owner in ("main", "member"):
            with self.subTest(owner=owner):
                f = distributed_activity_fixture()
                action = f.activity(display=f.member_tv, source=f.member_player, audio=f.member_avr).start()
                self.receive(f.member_tv, 4, 0x04)
                calls = []

                def snapshot():
                    self.assertFalse(f.session._lock._is_owned())
                    self.assertFalse(f.member_tv._power_condition._is_owned())
                    calls.append(None)
                    current = deepcopy(f.source.system)
                    if len(calls) > 1:
                        target = current if owner == "main" else current["membership"]["peers"][0]
                        target["boot_id"] = "e" * 32
                    return current

                f.dut.get_system = snapshot
                with self.assertRaises(AssertionError):
                    f.member_tv.wait_for_power("on", since=action, timeout=0.05)
                self.assertEqual(len(calls), 2)

    def test_initial_assertions_and_physical_activity_start_and_standby_commands(self):
        with self.fixture() as (dut, tv, avr, player, session):
            for component in (tv, avr, player):
                self.assertIs(component.assert_power(PowerState.STANDBY), component)
                self.assertEqual(component.power_events, ())
            start = self.boundary(dut, session)
            self.receive(tv, 4, 0x04)
            self.receive(avr, 0, 0x44, b"\x6d")
            self.receive(player, 0, 0x44, b"\x6d")
            for component in (tv, avr, player):
                self.assertIs(component.wait_for_power("on", since=start), component)
                self.assertEqual(component.power_assertions[-1]["kind"], "wire_command")
            stop = self.boundary(dut, session)
            for component, source in ((tv, 4), (avr, 0), (player, 0)):
                self.receive(component, source, 0x36)
                component.wait_for_power("standby", since=stop)
                self.assertEqual(component.power_events[-1]["command"], "standby")

    def test_valid_wire_commands_count_even_when_power_state_was_already_matching(self):
        with self.fixture() as (dut, tv, avr, player, session):
            avr.power_on()
            start = self.boundary(dut, session)
            self.receive(avr, 0, 0x44, b"\x6d")
            avr.wait_for_power("on", since=start)
            self.assertFalse(avr.power_events[-1]["state_changed"])
            stop = self.boundary(dut, session)
            self.receive(tv, 4, 0x36)
            tv.wait_for_power("standby", since=stop)
            self.assertFalse(tv.power_events[-1]["state_changed"])

    def test_explicit_standby_requires_opcode_36_instead_of_power_off_function(self):
        with self.fixture() as (dut, tv, avr, player, session):
            avr.power_on()
            stop = self.boundary(dut, session)
            self.receive(avr, 0, 0x44, b"\x6c")
            avr.wait_for_power("standby", since=stop)  # Existing generic behavior is retained.
            self.assertEqual(avr.power, PowerState.STANDBY)
            with self.assertRaises(TimeoutError):
                avr.wait_for_power("standby", since=stop, command="standby", timeout=0.005)
            self.receive(avr, 0, 0x36)
            avr.wait_for_power("standby", since=stop, command="standby")
            self.assertEqual(avr.power_assertions[-1]["command"], "standby")
            self.assertFalse(avr.power_events[-1]["state_changed"])

    def test_power_command_filter_rejects_unknown_names_and_types(self):
        with self.fixture() as (dut, tv, avr, player, session):
            stop = self.boundary(dut, session)
            for command in ("0x36", "Standby", "", 36, [], {}):
                with self.subTest(command=command), self.assertRaises(ValueError):
                    avr.wait_for_power("standby", since=stop, command=command)

    def test_local_and_virtual_power_changes_never_replace_physical_dut_command_proof(self):
        with self.fixture() as (dut, tv, avr, player, session):
            start = self.boundary(dut, session)
            avr.power_on()
            self.missing(avr, "on", start)
            avr.standby()
            player.send(avr.address.logical, 0x44, b"\x6d")
            self.assertEqual(avr.power, PowerState.ON)
            self.assertEqual(avr.power_events[-1]["transport"], "virtual")
            self.missing(avr, "on", start)

    def test_wrong_wire_source_and_unrelated_wake_commands_are_not_power_proof(self):
        with self.fixture() as (dut, tv, avr, player, session):
            start = self.boundary(dut, session)
            self.receive(tv, 8, 0x04)
            self.assertEqual(tv.power, PowerState.ON)
            self.missing(tv, "on", start)
            self.receive(player, 0, 0x41, b"\x24")  # Play wakes the model, but isn't a power command.
            self.assertEqual(player.power, PowerState.ON)
            self.assertEqual(player.power_events, ())
            self.missing(player, "on", start)
            self.receive(avr, 0, 0x70, b"\x11\x00")  # System Audio Mode also isn't proof of Power On.
            self.assertEqual(avr.power, PowerState.ON)
            self.assertEqual(avr.power_events, ())
            self.missing(avr, "on", start)

    def test_old_wire_time_cannot_be_made_fresh_by_a_receive_hook(self):
        with self.fixture() as (dut, tv, avr, player, session):
            old = time.monotonic_ns()
            start = self.boundary(dut, session)
            avr.hooks.before_rx = lambda component, frame: Frame(
                frame.source, frame.destination, frame.opcode, frame.operands,
                timestamp_ns=time.monotonic_ns() + 10**12)
            self.receive(avr, 0, 0x44, b"\x6d", timestamp=old)
            self.assertEqual(avr.power_events[-1]["wire_timestamp_ns"], old)
            self.missing(avr, "on", start)

    def test_dropped_rewritten_and_malformed_received_frames_do_not_count(self):
        with self.fixture() as (dut, tv, avr, player, session):
            start = self.boundary(dut, session)
            avr.hooks.before_rx = lambda component, frame: None
            self.receive(avr, 0, 0x44, b"\x6d")
            self.assertEqual(avr.power_events, ())
            avr.hooks.before_rx = lambda component, frame: Frame(0, 5, 0x44, b"\x6d", timestamp_ns=frame.timestamp_ns)
            self.receive(avr, 0, 0x8f)
            self.assertEqual(avr.power, PowerState.ON)
            self.assertEqual(avr.power_events, ())
            self.missing(avr, "on", start)
            avr.hooks = Hooks()
            avr.standby()
            for opcode, operands in ((0x44, b"\x6d\x00"), (0x36, b"\x00")):
                self.receive(avr, 0, opcode, operands)
            self.receive(tv, 4, 0x04, b"\x00")
            self.assertEqual(avr.power_events, ())
            self.assertEqual(tv.power_events, ())

    def test_a_later_local_state_restore_cannot_reuse_an_earlier_wire_command(self):
        with self.fixture() as (dut, tv, avr, player, session):
            start = self.boundary(dut, session)
            self.receive(avr, 0, 0x44, b"\x6d")
            avr.standby()
            avr.power_on()
            self.assertEqual(avr.power, PowerState.ON)
            self.missing(avr, "on", start)

    def test_ignored_same_state_command_and_hook_override_do_not_count_as_applied(self):
        with self.fixture() as (dut, tv, avr, player, session):
            avr.power_on()
            start = self.boundary(dut, session)
            original = avr.handle
            try:
                avr.handle = lambda frame: None
                self.receive(avr, 0, 0x44, b"\x6d")
            finally:
                avr.handle = original
            self.assertFalse(avr.power_events[-1]["applied"])
            self.missing(avr, "on", start)
            avr.standby()

            def overwrite(component, frame):
                component.standby()
                component.power_on()

            avr.hooks.after_rx = overwrite
            self.receive(avr, 0, 0x44, b"\x6d")
            self.assertFalse(avr.power_events[-1]["applied"])
            self.missing(avr, "on", start)

    def test_toggle_text_view_on_and_power_off_function_use_actual_handler_state(self):
        with self.fixture() as (dut, tv, avr, player, session):
            for key in (0x40, 0x6b):
                avr.standby()
                start = self.boundary(dut, session)
                self.receive(avr, 0, 0x44, bytes([key]))
                avr.wait_for_power("on", since=start)
                stop = self.boundary(dut, session)
                self.receive(avr, 0, 0x44, bytes([key]))
                avr.wait_for_power("standby", since=stop)
            start = self.boundary(dut, session)
            self.receive(tv, 4, 0x0d)
            tv.wait_for_power("on", since=start)
            stop = self.boundary(dut, session)
            self.receive(tv, 4, 0x44, b"\x6c")
            tv.wait_for_power("standby", since=stop)

    def test_power_off_removes_receiver_and_rejects_waiting_for_a_cec_off_command(self):
        with self.fixture() as (dut, tv, avr, player, session):
            tv.power_off()
            tv.assert_power("off")
            self.receive(tv, 4, 0x04)
            self.assertEqual(tv.power_events, ())
            self.assertEqual(tv.power, PowerState.OFF)
            with self.assertRaises(ValueError):
                tv.wait_for_power("off", since=self.boundary(dut, session))

    def test_foreign_boundaries_unverified_sources_and_invalid_timeouts_fail_closed(self):
        with self.fixture() as (dut, tv, avr, player, session):
            start = self.boundary(dut, session)
            for changed in (dict(dut=object()), dict(session=object()), dict(initiators={tv.gpio: 0})):
                wrong = SimpleNamespace(**{**vars(start), **changed})
                with self.subTest(changed=changed), self.assertRaises(AssertionError):
                    tv.wait_for_power("on", since=wrong)
            for timeout in (0, -1, True, float("nan"), float("inf")):
                with self.subTest(timeout=timeout), self.assertRaises(ValueError):
                    tv.wait_for_power("on", since=start, timeout=timeout)
        with self.assertRaises(AssertionError):
            tv.wait_for_power("on", since=start)

    def test_checkpoint_and_managed_timeout_work_without_user_polling_or_hooks(self):
        with use_config(RunConfig(cec_timeout=0.01)), self.fixture() as (dut, tv, avr, player, session):
            marker = tv.checkpoint()
            with self.assertRaises(TimeoutError):
                tv.wait_for_power("on", since=marker)
            marker = tv.checkpoint()
            self.receive(tv, 4, 0x04)
            tv.wait_for_power("on", since=marker)

    def test_long_operation_timeout_keeps_missing_cec_power_wait_at_five_seconds(self):
        with use_config(RunConfig(timeout=180)), self.fixture() as (dut, tv, avr, player, session):
            marker = tv.checkpoint()
            clock = [10.0]

            def wait(seconds):
                clock[0] += seconds

            with patch("amity_harness.components.time.monotonic", side_effect=lambda: clock[0]), \
                    patch.object(tv._power_condition, "wait", side_effect=wait):
                with self.assertRaises(TimeoutError):
                    tv.wait_for_power("on", since=marker)
            self.assertEqual(dut._runtime.timeout, 180)
            self.assertGreaterEqual(clock[0] - 10, 5)
            self.assertLess(clock[0] - 10, 5.1)

    def test_wait_wakes_for_background_wire_delivery_and_keeps_bounded_snapshot_evidence(self):
        with self.fixture() as (dut, tv, avr, player, session):
            start = self.boundary(dut, session)

            def later():
                time.sleep(0.01)
                self.receive(tv, 4, 0x04)

            worker = threading.Thread(target=later)
            worker.start()
            try:
                tv.wait_for_power("on", since=start, timeout=0.5)
            finally:
                worker.join()
            for _ in range(300):
                self.receive(avr, 0, 0x36)
                avr.assert_power("standby")
            self.assertEqual(len(avr.power_events), 256)
            self.assertEqual(len(avr.power_assertions), 256)
            snapshot = avr.power_events
            snapshot[-1]["command"] = "tampered"
            self.assertEqual(avr.power_events[-1]["command"], "standby")


if __name__ == "__main__":
    unittest.main()
