"""Routing assertions require a fresh, applied physical DUT command."""

from contextlib import contextmanager
from pathlib import Path
import queue
import sys
import threading
import time
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import (AvrComponent, CecSession, DUT, Frame, Hooks,
                           PlaybackComponent, PowerState, RunConfig, TvComponent,
                           TestBed, TxResult, use_config)


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


class QueuedWire(IdleWire):
    def __init__(self, gpio):
        super().__init__(gpio)
        self.frames = queue.Queue()

    def receive(self, timeout):
        try:
            return self.frames.get(timeout=timeout)
        except queue.Empty:
            return None


class ComponentRoutingAssertionTests(unittest.TestCase):
    @contextmanager
    def fixture(self, *, backend_factory=IdleWire):
        dut = DUT("amity-dut")
        tentacle = dut.tentacle(0)
        tv = TvComponent(tentacle.front, "Routing TV")
        avr = AvrComponent(tentacle.back, "Routing AVR")
        player = PlaybackComponent(avr, "Routing Player", port=1)
        with CecSession([tv, avr, player], backend_factory=backend_factory) as session:
            dut.last_scan = SimpleNamespace(initiators={tv.gpio: 4, avr.gpio: 0}, _session=session)
            yield dut, tv, avr, player, session

    def boundary(self, dut, session, *, started=None):
        return SimpleNamespace(dut=dut, session=session,
                               started_ns=time.monotonic_ns() if started is None else started,
                               initiators=dict(dut.last_scan.initiators))

    def receive(self, component, source, physical, *, opcode=0x86, destination=15,
                timestamp=None, transport="wire", operands=None):
        component.receive(Frame(source, destination, opcode,
                                physical.to_bytes(2, "big") if operands is None else operands,
                                timestamp_ns=time.monotonic_ns() if timestamp is None else timestamp),
                          transport=transport)

    def missing(self, component, physical, boundary):
        with self.assertRaises(TimeoutError):
            component.wait_for_input(physical, since=boundary, timeout=0.005)

    def test_fresh_physical_set_stream_path_proves_tv_and_avr_routing(self):
        with self.fixture() as (dut, tv, avr, player, session):
            start = self.boundary(dut, session)
            for component, source in ((tv, 4), (avr, 0)):
                self.receive(component, source, player.address.physical)
                self.assertIs(component.wait_for_input(player, since=start), component)
                self.assertEqual(component.active_input, player.address.physical)
                self.assertEqual(component.routing_events[-1]["transport"], "wire")
                self.assertTrue(component.routing_events[-1]["applied"])
                self.assertEqual(component.routing_assertions[-1]["frame"],
                                 Frame(source, 15, 0x86, player.address.physical.to_bytes(2, "big")).raw.hex(":"))

    def test_same_state_physical_command_counts_when_the_handler_applies_it(self):
        with self.fixture() as (dut, tv, avr, player, session):
            avr.active_input = player.address.physical
            start = self.boundary(dut, session)
            self.receive(avr, 0, player.address.physical)
            avr.wait_for_input(player.address.physical, since=start)
            self.assertTrue(avr.routing_events[-1]["applied"])

    def switch_command(self, component, *, source=0, number=4, destination=None,
                       operands=None, timestamp=None, transport="wire"):
        component.receive(Frame(source, component.address.logical if destination is None else destination,
                                0x44, bytes((0x69, number)) if operands is None else operands,
                                timestamp_ns=time.monotonic_ns() if timestamp is None else timestamp),
                          transport=transport)

    def missing_switch(self, component, boundary, number=4):
        with self.assertRaises(TimeoutError):
            component.wait_for_switch_input(number, since=boundary, timeout=0.005)

    def test_switch_input_requires_applied_directed_command_and_remains_distinct_from_physical_route(self):
        with self.fixture() as (dut, tv, avr, player, session):
            start = self.boundary(dut, session)
            self.receive(avr, 0, player.address.physical)
            self.switch_command(avr)
            self.assertIs(avr.wait_for_switch_input(4, since=start), avr)
            self.assertEqual(avr.selected_input, 4)
            self.assertEqual(avr.active_input, player.address.physical)
            proof = avr.routing_assertions[-1]
            self.assertEqual(proof["frame"], "05:44:69:04")
            self.assertEqual(proof["destination"], avr.address.logical)
            self.assertEqual(proof["input"], 4)
            self.assertEqual(proof["command"], "select_av_input")
            report = TestBed(dut, tv, avr, player).report
            reported_avr = next(component for component in report["components"] if component["name"] == avr.name)
            self.assertEqual(reported_avr["selected_input"], 4)
            self.assertEqual(reported_avr["routing_assertions"][-1], proof)
            self.assertEqual(reported_avr["routing_events"][-1]["input"], 4)
            # The finite input key is released without clearing the selection.
            avr.receive(Frame(0, avr.address.logical, 0x45, timestamp_ns=time.monotonic_ns()))
            self.assertIsNone(avr.pressed_key)
            self.assertEqual(avr.selected_input, 4)

    def test_tv_can_select_numeric_input_and_same_input_command_still_counts(self):
        with self.fixture() as (dut, tv, avr, player, session):
            tv.selected_input = 4
            start = self.boundary(dut, session)
            self.switch_command(tv, source=4)
            self.assertIs(tv.wait_for_switch_input(4, since=start), tv)

    def test_local_virtual_stale_or_other_input_commands_cannot_prove_requested_switch_input(self):
        with self.fixture() as (dut, tv, avr, player, session):
            old = time.monotonic_ns()
            start = self.boundary(dut, session)
            avr.selected_input = 4
            self.missing_switch(avr, start)
            self.receive(avr, 0, player.address.physical)
            self.missing_switch(avr, start)
            self.switch_command(avr, transport="virtual")
            self.missing_switch(avr, start)
            self.switch_command(avr, timestamp=old)
            self.missing_switch(avr, start)
            self.switch_command(avr, number=3)
            self.missing_switch(avr, start)

    def test_wrong_switch_sender_destination_or_operands_cannot_satisfy_assertion(self):
        cases = (dict(source=8), dict(source=15), dict(destination=15), dict(destination=4),
                 dict(operands=b""), dict(operands=b"\x69"), dict(operands=b"\x69\x04\x01"),
                 dict(operands=b"\x6d\x04"))
        for arguments in cases:
            with self.subTest(arguments=arguments), self.fixture() as (dut, tv, avr, player, session):
                start = self.boundary(dut, session)
                self.switch_command(avr, **arguments)
                self.missing_switch(avr, start)

    def test_switch_hooks_and_ignored_commands_cannot_fabricate_applied_wire_evidence(self):
        with self.fixture() as (dut, tv, avr, player, session):
            start = self.boundary(dut, session)
            avr.hooks.before_rx = lambda component, frame: None
            self.switch_command(avr)
            self.missing_switch(avr, start)
            avr.hooks.before_rx = lambda component, frame: Frame(
                frame.source, frame.destination, frame.opcode, b"\x69\x04", timestamp_ns=frame.timestamp_ns)
            self.switch_command(avr, number=3)
            self.assertEqual(avr.selected_input, 4)
            self.missing_switch(avr, start)
            avr.hooks = Hooks()
            handle = avr.handle
            avr.handle = lambda frame: None
            try:
                self.switch_command(avr)
            finally:
                avr.handle = handle
            self.assertFalse(avr.routing_events[-1]["applied"])
            self.missing_switch(avr, start)

    def test_switch_input_changed_and_restored_invalidates_old_evidence(self):
        with self.fixture() as (dut, tv, avr, player, session):
            start = self.boundary(dut, session)

            def replace(component, frame):
                component.selected_input = 3
                component.selected_input = 4

            avr.hooks.after_rx = replace
            self.switch_command(avr)
            self.assertFalse(avr.routing_events[-1]["applied"])
            self.missing_switch(avr, start)
            avr.hooks = Hooks()
            self.switch_command(avr)
            avr.wait_for_switch_input(4, since=start)
            avr.selected_input = 3
            avr.selected_input = 4
            self.missing_switch(avr, start)

    def test_unpowered_switch_has_no_rx_or_routing_proof(self):
        with self.fixture() as (dut, tv, avr, player, session):
            start = self.boundary(dut, session)
            avr.power_off()
            self.switch_command(avr)
            self.assertIsNone(avr.selected_input)
            self.assertEqual(avr.routing_events, ())
            self.missing_switch(avr, start)

    def test_switch_input_and_boundary_validation(self):
        with self.fixture() as (dut, tv, avr, player, session):
            start = self.boundary(dut, session)
            for number in (None, True, -1, 256, 4.0, "4"):
                with self.subTest(number=number), self.assertRaises(ValueError):
                    avr.wait_for_switch_input(number, since=start)
            for number in (0, 255):
                self.switch_command(avr, number=number)
                avr.wait_for_switch_input(number, since=start)
            for change in (dict(dut=object()), dict(session=object()), dict(initiators={avr.gpio: 4})):
                with self.subTest(change=change), self.assertRaises(AssertionError):
                    avr.wait_for_switch_input(4, since=SimpleNamespace(**{**vars(start), **change}))
            with self.assertRaises(ValueError):
                player.wait_for_switch_input(4, since=start)

    def test_same_path_playback_announcement_preserves_applied_wire_routing_proof(self):
        backends = {}

        def factory(gpio):
            backend = backends[gpio] = QueuedWire(gpio)
            return backend

        with self.fixture(backend_factory=factory) as (dut, tv, avr, player, session):
            avr.active_input = 0x1200
            start = self.boundary(dut, session)
            announced = threading.Event()

            def after_rx(component, frame):
                if frame.source == player.address.logical and frame.opcode == 0x82:
                    announced.set()

            avr.hooks.after_rx = after_rx
            backends[avr.gpio].frames.put(Frame(0, 15, 0x86,
                                               player.address.physical.to_bytes(2, "big"),
                                               timestamp_ns=time.monotonic_ns()))
            self.assertTrue(announced.wait(0.5), "The selected player did not announce its restored input")
            self.assertIs(avr.wait_for_input(player, since=start, timeout=0.05), avr)
            self.assertTrue(player.active)
            self.assertEqual(player.power, PowerState.ON)
            self.assertEqual(avr.active_input, player.address.physical)
            self.assertEqual(avr.routing_events[-1]["transport"], "virtual")
            self.assertEqual(avr.routing_events[-1]["opcode"], 0x82)

    def test_routing_wait_finishes_only_after_the_worker_delivers_to_playback(self):
        backends = {}

        def factory(gpio):
            backend = backends[gpio] = QueuedWire(gpio)
            return backend

        with self.fixture(backend_factory=factory) as (dut, tv, avr, player, session):
            start = self.boundary(dut, session)
            playback_blocked, release_playback = threading.Event(), threading.Event()
            waiting, completed = threading.Event(), threading.Event()
            errors, active_at_return = [], []

            def before_playback(component, frame):
                if frame.source == 0 and frame.opcode == 0x86:
                    playback_blocked.set()
                    if not release_playback.wait(1):
                        raise AssertionError("The test did not release playback handling")
                return frame

            def wait_for_route():
                waiting.set()
                try:
                    avr.wait_for_input(player, since=start, timeout=0.5)
                    active_at_return.append(player.active)
                except BaseException as error:
                    errors.append(error)
                finally:
                    completed.set()

            player.hooks.before_rx = before_playback
            backends[avr.gpio].frames.put(Frame(0, 15, 0x86,
                                               player.address.physical.to_bytes(2, "big"),
                                               timestamp_ns=time.monotonic_ns()))
            self.assertTrue(playback_blocked.wait(0.5))
            self.assertEqual(avr.routing_events[-1]["opcode"], 0x86)
            self.assertFalse(player.active)
            timeout_started = time.monotonic()
            with self.assertRaises(TimeoutError):
                avr.wait_for_input(player, since=start, timeout=0.01)
            self.assertLess(time.monotonic() - timeout_started, 0.2,
                            "The worker completion barrier ignored the assertion deadline")
            waiter = threading.Thread(target=wait_for_route)
            waiter.start()
            try:
                self.assertTrue(waiting.wait(0.5))
                self.assertFalse(completed.wait(0.03), "Routing was exposed before playback handled the frame")
            finally:
                release_playback.set()
                waiter.join(1)
            self.assertFalse(waiter.is_alive())
            self.assertTrue(completed.is_set())
            self.assertEqual(errors, [])
            self.assertEqual(active_at_return, [True])

    def test_activate_wakes_generic_components_and_returns_actual_wire_result(self):
        with self.fixture() as (dut, tv, avr, player, session):
            for component in (avr, player):
                self.assertEqual(component.power, PowerState.STANDBY)
                result = component.activate()
                self.assertIsInstance(result, TxResult)
                self.assertTrue(result.ok)
                self.assertEqual(result.transport, "wire")
                self.assertEqual(result.frame.raw,
                                 Frame(component.address.logical, 15, 0x82,
                                       component.address.physical.to_bytes(2, "big")).raw)
                self.assertEqual(component.power, PowerState.ON)
            self.assertTrue(player.active)

    def test_directed_power_assertion_rejects_broadcast_standby_but_accepts_addressed_command(self):
        with self.fixture() as (dut, tv, avr, player, session):
            avr.power_on()
            start = self.boundary(dut, session)
            avr.receive(Frame(0, 15, 0x36, timestamp_ns=time.monotonic_ns()))
            self.assertEqual(avr.power, PowerState.STANDBY)
            avr.wait_for_power("standby", since=start, command="standby")
            with self.assertRaises(TimeoutError):
                avr.wait_for_power("standby", since=start, command="standby", directed=True, timeout=0.005)
            avr.receive(Frame(0, avr.address.logical, 0x36, timestamp_ns=time.monotonic_ns()))
            self.assertIs(avr.wait_for_power("standby", since=start, command="standby", directed=True), avr)
            self.assertEqual(avr.power_events[-1]["destination"], avr.address.logical)

    def test_local_input_and_virtual_source_or_stream_messages_are_not_dut_proof(self):
        with self.fixture() as (dut, tv, avr, player, session):
            start = self.boundary(dut, session)
            avr.active_input = player.address.physical
            self.missing(avr, player, start)
            avr.active_input = None
            player.activate()
            self.assertEqual(avr.active_input, player.address.physical)
            self.assertEqual(avr.routing_events[-1]["transport"], "virtual")
            self.missing(avr, player, start)
            avr.active_input = None
            player.send(15, 0x86, player.address.physical.to_bytes(2, "big"))
            self.assertEqual(avr.active_input, player.address.physical)
            self.missing(avr, player, start)
            # Isolate provenance from initiator matching: even a virtual frame
            # carrying the DUT's source address cannot prove physical routing.
            self.receive(avr, 0, player.address.physical, transport="virtual")
            self.missing(avr, player, start)

    def test_wrong_source_destination_address_and_opcode_fail_closed(self):
        cases = ((8, 15, 0x86, 0x1100), (15, 15, 0x86, 0x1100),
                 (0, 5, 0x86, 0x1100), (0, 4, 0x86, 0x1100),
                 (0, 15, 0x86, 0x1200), (0, 15, 0x82, 0x1100))
        for source, destination, opcode, physical in cases:
            with self.subTest(source=source, destination=destination, opcode=opcode, physical=physical):
                with self.fixture() as (dut, tv, avr, player, session):
                    start = self.boundary(dut, session)
                    self.receive(avr, source, physical, opcode=opcode, destination=destination)
                    self.missing(avr, player, start)

    def test_malformed_stream_path_operands_do_not_count(self):
        for operands in (b"", b"\x11", b"\x11\x00\x00"):
            with self.subTest(operands=operands), self.fixture() as (dut, tv, avr, player, session):
                start = self.boundary(dut, session)
                self.receive(avr, 0, player.address.physical, operands=operands)
                self.assertIsNone(avr.active_input)
                self.missing(avr, player, start)

    def test_stale_wire_packet_cannot_become_fresh_when_processed_after_the_boundary(self):
        with self.fixture() as (dut, tv, avr, player, session):
            old = time.monotonic_ns()
            start = self.boundary(dut, session)
            self.receive(avr, 0, player.address.physical, timestamp=old)
            self.assertEqual(avr.active_input, player.address.physical)
            self.missing(avr, player, start)

    def test_timestamp_rewrite_preserves_original_wire_provenance(self):
        with self.fixture() as (dut, tv, avr, player, session):
            old = time.monotonic_ns()
            start = self.boundary(dut, session)
            avr.hooks.before_rx = lambda component, frame: Frame(
                frame.source, frame.destination, frame.opcode, frame.operands,
                timestamp_ns=time.monotonic_ns() + 10**12)
            self.receive(avr, 0, player.address.physical, timestamp=old)
            self.assertEqual(avr.routing_events[-1]["wire_timestamp_ns"], old)
            self.missing(avr, player, start)

    def test_dropped_and_rewritten_frames_cannot_fabricate_dut_routing(self):
        with self.fixture() as (dut, tv, avr, player, session):
            start = self.boundary(dut, session)
            avr.hooks.before_rx = lambda component, frame: None
            self.receive(avr, 0, player.address.physical)
            self.assertIsNone(avr.active_input)
            self.missing(avr, player, start)
            avr.hooks.before_rx = lambda component, frame: Frame(
                0, 15, 0x86, player.address.physical.to_bytes(2, "big"),
                timestamp_ns=frame.timestamp_ns)
            self.receive(avr, 8, 0x1200, opcode=0x82)
            self.assertEqual(avr.active_input, player.address.physical)
            self.missing(avr, player, start)
            # The original wire frame now has every expected field, but a hook
            # substitutes Active Source before handling it. State alone cannot
            # prove that Set Stream Path was actually applied.
            avr.hooks.before_rx = lambda component, frame: Frame(
                frame.source, frame.destination, 0x82, frame.operands,
                timestamp_ns=frame.timestamp_ns)
            self.receive(avr, 0, player.address.physical)
            self.assertEqual(avr.active_input, player.address.physical)
            self.assertFalse(avr.routing_events[-1]["applied"])
            self.missing(avr, player, start)

    def test_handler_must_apply_the_command_even_if_local_input_already_matches(self):
        with self.fixture() as (dut, tv, avr, player, session):
            avr.active_input = player.address.physical
            start = self.boundary(dut, session)
            original = avr.handle
            try:
                avr.handle = lambda frame: None
                self.receive(avr, 0, player.address.physical)
            finally:
                avr.handle = original
            self.assertFalse(avr.routing_events[-1]["applied"])
            self.missing(avr, player, start)

    def test_after_hook_override_and_later_local_restore_invalidate_old_command_proof(self):
        with self.fixture() as (dut, tv, avr, player, session):
            start = self.boundary(dut, session)

            def overwrite(component, frame):
                component.active_input = 0x1200
                component.active_input = player.address.physical

            avr.hooks.after_rx = overwrite
            self.receive(avr, 0, player.address.physical)
            self.assertEqual(avr.active_input, player.address.physical)
            self.assertFalse(avr.routing_events[-1]["applied"])
            self.missing(avr, player, start)
            avr.hooks = Hooks()
            self.receive(avr, 0, player.address.physical)
            avr.wait_for_input(player, since=start)
            avr.active_input = 0x1200
            avr.active_input = player.address.physical
            self.missing(avr, player, start)

    def test_unpowered_receiver_cannot_prove_or_apply_routing(self):
        with self.fixture() as (dut, tv, avr, player, session):
            start = self.boundary(dut, session)
            avr.power_off()
            self.receive(avr, 0, player.address.physical)
            self.assertIsNone(avr.active_input)
            self.assertEqual(avr.routing_events, ())
            self.missing(avr, player, start)

    def test_foreign_boundaries_unverified_initiators_and_invalid_arguments_fail_closed(self):
        with self.fixture() as (dut, tv, avr, player, session):
            start = self.boundary(dut, session)
            for changed in (dict(dut=object()), dict(session=object()), dict(initiators={avr.gpio: 4})):
                wrong = SimpleNamespace(**{**vars(start), **changed})
                with self.subTest(changed=changed), self.assertRaises(AssertionError):
                    avr.wait_for_input(player, since=wrong)
            for timeout in (0, -1, True, float("nan"), float("inf")):
                with self.subTest(timeout=timeout), self.assertRaises(ValueError):
                    avr.wait_for_input(player, since=start, timeout=timeout)
            for physical in (None, True, -1, 0xffff, 0x10000, 0x1010, 1.0):
                with self.subTest(physical=physical), self.assertRaises((ValueError, TypeError)):
                    avr.wait_for_input(physical, since=start, timeout=0.005)
        with self.assertRaises(AssertionError):
            avr.wait_for_input(player.address.physical, since=start)

    def test_checkpoint_and_managed_timeout_need_no_user_polling(self):
        with use_config(RunConfig(timeout=0.01)), self.fixture() as (dut, tv, avr, player, session):
            marker = avr.checkpoint()
            with self.assertRaises(TimeoutError):
                avr.wait_for_input(player, since=marker)
            marker = avr.checkpoint()
            self.receive(avr, 0, player.address.physical)
            avr.wait_for_input(player, since=marker)

    def test_background_delivery_and_bounded_immutable_evidence_snapshots(self):
        with self.fixture() as (dut, tv, avr, player, session):
            start = self.boundary(dut, session)

            def later():
                time.sleep(0.01)
                self.receive(avr, 0, player.address.physical)

            worker = threading.Thread(target=later)
            worker.start()
            try:
                avr.wait_for_input(player, since=start, timeout=0.5)
            finally:
                worker.join()
            for _ in range(300):
                self.receive(avr, 0, player.address.physical)
                avr.wait_for_input(player, since=start)
            self.assertEqual(len(avr.routing_events), 256)
            self.assertEqual(len(avr.routing_assertions), 256)
            snapshot = avr.routing_events
            snapshot[-1]["frame"] = "tampered"
            self.assertNotEqual(avr.routing_events[-1]["frame"], "tampered")
            snapshot = avr.routing_assertions
            snapshot[-1]["frame"] = "tampered"
            self.assertNotEqual(avr.routing_assertions[-1]["frame"], "tampered")


if __name__ == "__main__":
    unittest.main()
