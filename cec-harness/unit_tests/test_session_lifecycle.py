"""Host-only checks of wire sharing, power transitions and fixture cleanup."""

from pathlib import Path
import queue
import sys
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import (
    AvrComponent, CecError, CecSession, DUT, Frame, Hooks, LinuxCecBackend,
    PlaybackComponent, PowerState, TvComponent, TxResult, assign_addresses,
)


class Backend:
    def __init__(self, gpio):
        self.gpio = gpio
        self.frames = queue.Queue()
        self.sent = []
        self.claimed = []
        self.closed = False
        self.tx_status = 1
        self.fail_configure = False
        self.fail_close = False

    def open(self):
        pass

    def configure(self, components):
        self.claimed = list(components)
        if self.fail_configure:
            self.claimed = []
            raise OSError("injected address-claim failure")

    def transmit(self, frame):
        self.sent.append(frame)
        return TxResult(frame, self.tx_status, time.monotonic_ns())

    def receive(self, timeout):
        try:
            return self.frames.get(timeout=timeout)
        except queue.Empty:
            return None

    def close(self):
        self.claimed = []
        self.closed = True
        if self.fail_close:
            raise OSError("injected cleanup failure")


class ObservedLock:
    """Signal when a power-off thread actually reaches the shared session lock."""

    def __init__(self):
        self.lock = threading.RLock()
        self.power_attempt = threading.Event()

    def __enter__(self):
        if threading.current_thread().name == "power-off-test":
            self.power_attempt.set()
        self.lock.acquire()
        return self

    def __exit__(self, *args):
        self.lock.release()


class SessionLifecycleTests(unittest.TestCase):
    def setUp(self):
        self.dut = DUT("amity-dut")
        self.backends = {}

    def backend(self, gpio):
        result = Backend(gpio)
        self.backends[gpio] = result
        return result

    def topology(self, **player_args):
        tv = TvComponent(self.dut, "Test TV")
        avr = AvrComponent(self.dut.tentacle(0).back, "Test AVR")
        player = PlaybackComponent(avr, "Test Player", port=1, **player_args)
        return tv, avr, player

    def test_successful_tx_reaches_same_wire_siblings_once(self):
        tv, avr, player = self.topology()
        seen = []
        avr.hooks = Hooks(after_rx=lambda component, frame: seen.append(frame))
        with CecSession([tv, avr, player], backend_factory=self.backend):
            seen.clear()
            player.activate()
            self.assertEqual(avr.active_input, player.address.physical)
            self.assertEqual([frame.opcode for frame in seen], [0x82])
            self.assertIsNone(tv.active_input)  # It is on the isolated front wire.

    def test_failed_wire_tx_still_updates_virtual_sibling_model(self):
        tv, avr, player = self.topology()
        with CecSession([tv, avr, player], backend_factory=self.backend):
            self.backends[player.gpio].tx_status = 4
            player.activate()
            self.assertEqual(avr.active_input, player.address.physical)

    def test_off_sibling_has_no_address_or_receive_callbacks(self):
        tv, avr, player = self.topology()
        seen = []
        avr.hooks = Hooks(before_rx=lambda component, frame: seen.append(frame) or frame)
        with CecSession([tv, avr, player], backend_factory=self.backend):
            avr.power_off()
            seen.clear()
            player.activate()
            self.assertNotIn(avr, self.backends[avr.gpio].claimed)
            self.assertFalse(seen)
            self.assertIsNone(avr.active_input)
            with self.assertRaises(CecError):
                avr.send(15, 0x36)

    def test_rx_fault_hook_can_cut_power_before_handling(self):
        tv = TvComponent(self.dut, "Test TV")
        tv.hooks = Hooks(before_rx=lambda component, frame: component.power_off() or frame)
        with CecSession([tv], backend_factory=self.backend) as session:
            # Image View On would wake a standby TV if processing continued.
            tv.receive(Frame(4, 0, 0x04))
            self.assertEqual(tv.power, PowerState.OFF)
            self.assertFalse(self.backends[tv.gpio].claimed)
            self.assertFalse(any(record["direction"] == "rx" and not record.get("dropped")
                                 for record in session.records))

    def test_power_off_is_atomic_with_an_inflight_reply(self):
        tv = TvComponent(self.dut, "Test TV", power=PowerState.ON)
        in_reply, release_reply = threading.Event(), threading.Event()

        def pause_reply(component, frame):
            if frame.opcode == 0x47:
                in_reply.set()
                if not release_reply.wait(3):
                    raise RuntimeError("test did not release reply")
            return frame

        tv.hooks = Hooks(before_tx=pause_reply)
        session = CecSession([tv], backend_factory=self.backend)
        observed = ObservedLock()
        session._lock = observed
        errors = []

        def power_off():
            try:
                tv.power_off()
            except BaseException as error:
                errors.append(error)

        with session:
            self.backends[tv.gpio].frames.put(Frame(4, 0, 0x46))
            self.assertTrue(in_reply.wait(3))
            worker = threading.Thread(target=power_off, name="power-off-test")
            worker.start()
            try:
                self.assertTrue(observed.power_attempt.wait(3))
                # The responder still owns the lock and has not submitted its
                # reply. Power must remain on until that causal work finishes.
                power_before_reply = tv.power
            finally:
                release_reply.set()
                worker.join(3)
            self.assertEqual(power_before_reply, PowerState.ON)
            self.assertFalse(worker.is_alive())
            self.assertFalse(errors)
            session.assert_healthy()
            self.assertEqual(tv.power, PowerState.OFF)

    def test_failed_power_reconfiguration_fences_session_and_cleans_up(self):
        tv = TvComponent(self.dut, "Test TV", power=PowerState.ON)
        session = CecSession([tv], backend_factory=self.backend)
        session.__enter__()
        try:
            self.backends[tv.gpio].fail_configure = True
            with self.assertRaises(OSError):
                tv.power_off()
            with self.assertRaises(CecError):
                session.assert_healthy()
        finally:
            session.close()
        self.assertTrue(self.backends[tv.gpio].closed)
        self.assertIsNone(tv._session)

    def test_cleanup_attempts_every_backend_even_after_one_failure(self):
        tv, avr, player = self.topology()
        with self.assertRaises(CecError):
            with CecSession([tv, avr, player], backend_factory=self.backend):
                self.backends[player.gpio].fail_close = True
        self.assertTrue(all(backend.closed for backend in self.backends.values()))
        self.assertTrue(all(component._session is None for component in (tv, avr, player)))

    def test_failed_second_session_does_not_steal_component(self):
        tv = TvComponent(self.dut, "Test TV")
        with CecSession([tv], backend_factory=self.backend) as owner:
            with self.assertRaises(CecError):
                with CecSession([tv], backend_factory=self.backend):
                    self.fail("second session should reject the owned component")
            self.assertIs(tv._session, owner)
            owner.assert_healthy()
            self.assertTrue(tv.send(15, 0x36).ok)

    def test_playback_first_shared_wire_keeps_tv_at_physical_root(self):
        tv = TvComponent(self.dut, "Test TV")
        player = PlaybackComponent(tv, "Test Player", port=1)
        assign_addresses([player, tv])
        backend = LinuxCecBackend(tv.gpio)
        backend.fd = 123  # All adapter operations below are intercepted.
        backend.caps = SimpleNamespace(available_log_addrs=4)
        adapter_pa, claimed = [], []

        def kernel_ioctl(fd, direction, number, value):
            self.assertEqual(fd, 123)
            if number == 2:
                adapter_pa.append(value.value)
            elif number == 4 and value.num_log_addrs:
                for index in range(value.num_log_addrs):
                    logical_type = value.log_addr_type[index]
                    # The real kernel uses Specific LA14 for TV functionality
                    # when the adapter has a nonzero physical address.
                    logical = 0 if logical_type == 0 and adapter_pa[-1] == 0 else 14 if logical_type == 0 else 4
                    value.log_addr[index] = logical
                    value.log_addr_mask |= 1 << logical
                    claimed.append(logical)
            return value

        with patch("amity_harness.cec._ioctl", side_effect=kernel_ioctl):
            backend.configure([player, tv])
        self.assertEqual(adapter_pa, [0])
        self.assertEqual(claimed, [4, 0])


if __name__ == "__main__":
    unittest.main()
