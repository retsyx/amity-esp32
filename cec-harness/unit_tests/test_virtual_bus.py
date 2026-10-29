"""Host-only checks of component-to-component CEC delivery inside a fixture."""

from pathlib import Path
import sys
import threading
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import (
    AvrComponent, CecAddress, CecError, CecSession, DUT, Frame, Hooks,
    PlaybackComponent, PowerState, TvComponent, TxResult,
)


class Backend:
    def __init__(self, gpio):
        self.gpio = gpio
        self.sent = []
        self.claimed = []
        self.status = 1
        self.closed = False
        self.idle = threading.Event()

    def open(self):
        pass

    def configure(self, components):
        self.claimed = list(components)

    def transmit(self, frame):
        self.sent.append(frame)
        return TxResult(frame, self.status, time.monotonic_ns())

    def receive(self, timeout):
        self.idle.wait(timeout)
        return None

    def close(self):
        self.claimed = []
        self.closed = True


class VirtualBusTests(unittest.TestCase):
    def setUp(self):
        self.dut = DUT("amity-dut")
        self.backends = {}

    def backend(self, gpio):
        backend = Backend(gpio)
        self.backends[gpio] = backend
        return backend

    def topology(self):
        tv = TvComponent(self.dut, "Virtual TV")
        avr = AvrComponent(self.dut.tentacle(0).back, "Virtual AVR")
        player = PlaybackComponent(avr, "Virtual Player", port=1)
        return tv, avr, player

    def clear_wire(self):
        for backend in self.backends.values():
            backend.sent.clear()

    def assert_no_wire_tx(self):
        self.assertFalse(any(backend.sent for backend in self.backends.values()))

    def test_directed_peer_delivery_and_all_hooks_are_virtual(self):
        tv, avr, player = self.topology()
        seen = []
        player.hooks = Hooks(before_tx=lambda component, frame: seen.append("before_tx") or frame,
                             after_tx=lambda component, result: seen.append(("after_tx", result.transport)))
        avr.hooks = Hooks(before_rx=lambda component, frame: seen.append("before_rx") or frame,
                          after_rx=lambda component, frame: seen.append("after_rx"))
        with CecSession([tv, avr, player], backend_factory=self.backend) as session:
            self.clear_wire()
            seen.clear()
            result = player.send(avr.address.logical, 0x44, [0x41])
            self.assertTrue(result.ok)
            self.assertEqual(result.transport, "virtual")
            self.assertEqual(avr.volume, 26)
            self.assertCountEqual(seen, ["before_tx", "before_rx", "after_rx", ("after_tx", "virtual")])
            self.assertLess(seen.index("before_tx"), seen.index(("after_tx", "virtual")))
            self.assertLess(seen.index("before_rx"), seen.index("after_rx"))
            self.assert_no_wire_tx()
            traffic = [record for record in session.records if record["opcode"] == 0x44]
            self.assertEqual([record["direction"] for record in traffic], ["tx", "rx"])
            self.assertTrue(all(record["transport"] == "virtual" and record["wire_timestamp_ns"] == 0
                                for record in traffic))

    def test_peer_query_and_matching_reply_stay_inside_bus(self):
        tv, avr, player = self.topology()
        with CecSession([tv, avr, player], backend_factory=self.backend) as session:
            self.clear_wire()
            result = player.send(avr.address.logical, 0x46)
            self.assertTrue(result.ok)
            self.assert_no_wire_tx()
            traffic = [record for record in session.records
                       if record["opcode"] in (0x46, 0x47)]
            self.assertEqual([(record["direction"], record["opcode"], record["component"])
                              for record in traffic],
                             [("tx", 0x46, player.name), ("rx", 0x46, avr.name),
                              ("tx", 0x47, avr.name), ("rx", 0x47, player.name)])
            self.assertTrue(all(record["transport"] == "virtual" and record["wire_timestamp_ns"] == 0
                                for record in traffic))

    def test_peer_poll_is_virtual_ack_and_power_off_is_virtual_nack(self):
        tv, avr, player = self.topology()
        with CecSession([tv, avr, player], backend_factory=self.backend):
            self.clear_wire()
            awake = player.send(avr.address.logical)
            self.assertTrue(awake.ok)
            self.assertEqual(awake.transport, "virtual")
            self.assert_no_wire_tx()
            avr.power_off()
            self.assertNotIn(avr, self.backends[avr.gpio].claimed)
            self.clear_wire()
            asleep = player.send(avr.address.logical)
            self.assertFalse(asleep.ok)
            self.assertEqual(asleep.status, 4)
            self.assertEqual(asleep.transport, "virtual")
            self.assert_no_wire_tx()

    def test_directed_to_off_peer_is_nack_without_gpio_or_rx(self):
        tv, avr, player = self.topology()
        received = []
        avr.hooks = Hooks(before_rx=lambda component, frame: received.append(frame) or frame)
        with CecSession([tv, avr, player], backend_factory=self.backend):
            avr.power_off()
            received.clear()
            self.clear_wire()
            result = player.send(avr.address.logical, 0x44, [0x41])
            self.assertFalse(result.ok)
            self.assertEqual(result.transport, "virtual")
            self.assertEqual(avr.volume, 25)
            self.assertFalse(received)
            self.assert_no_wire_tx()

    def test_same_logical_address_on_other_gpio_cannot_capture_peer_traffic(self):
        front_tv = TvComponent(self.dut, "Front TV")
        back_tv = TvComponent(self.dut, "Back TV", direction="back")
        player = PlaybackComponent(front_tv, "Front Player", port=1)
        with CecSession([front_tv, player, back_tv], backend_factory=self.backend):
            self.clear_wire()
            result = player.send(0, 0x04)
            self.assertEqual(result.transport, "virtual")
            self.assertEqual(front_tv.power, PowerState.ON)
            self.assertEqual(back_tv.power, PowerState.STANDBY)
            self.assert_no_wire_tx()

    def test_external_dut_destination_still_uses_real_backend(self):
        tv, avr, player = self.topology()
        with CecSession([tv, avr, player], backend_factory=self.backend) as session:
            self.clear_wire()
            result = player.send(0, 0x04)  # DUT TV functionality on the back wire.
            self.assertTrue(result.ok)
            self.assertEqual(result.transport, "wire")
            self.assertEqual(len(self.backends[player.gpio].sent), 1)
            self.assertFalse(self.backends[tv.gpio].sent)
            self.assertEqual(tv.power, PowerState.STANDBY)
            traffic = [record for record in session.records if record["opcode"] == 0x04]
            self.assertEqual(len(traffic), 1)
            self.assertEqual(traffic[0]["transport"], "wire")
            self.assertGreater(traffic[0]["wire_timestamp_ns"], 0)

    def test_external_receive_keeps_actual_wire_timestamp(self):
        tv, avr, player = self.topology()
        with CecSession([tv, avr, player], backend_factory=self.backend) as session:
            timestamp = time.monotonic_ns()
            avr.receive(Frame(0, avr.address.logical, 0x44, bytes([0x41]), timestamp_ns=timestamp))
            self.assertEqual(avr.volume, 26)
            traffic = [record for record in session.records if record["opcode"] == 0x44]
            self.assertEqual(len(traffic), 1)
            self.assertEqual(traffic[0]["direction"], "rx")
            self.assertEqual(traffic[0]["transport"], "wire")
            self.assertEqual(traffic[0]["wire_timestamp_ns"], timestamp)

    def test_broadcast_reaches_peer_once_and_wire_even_if_wire_fails(self):
        tv, avr, player = self.topology()
        received = []
        avr.hooks = Hooks(after_rx=lambda component, frame: received.append(frame))
        with CecSession([tv, avr, player], backend_factory=self.backend) as session:
            self.clear_wire()
            received.clear()
            self.backends[player.gpio].status = 4
            result = player.send(15, 0x82, player.address.physical.to_bytes(2, "big"))
            self.assertFalse(result.ok)
            self.assertEqual(result.transport, "wire")
            self.assertEqual(len(self.backends[player.gpio].sent), 1)
            self.assertEqual(len(received), 1)
            self.assertEqual(avr.active_input, player.address.physical)
            self.assertIsNone(tv.active_input)
            traffic = [record for record in session.records if record["opcode"] == 0x82]
            self.assertEqual(sum(record["direction"] == "tx" and record["transport"] == "wire"
                                 for record in traffic), 1)
            self.assertEqual(sum(record["direction"] == "rx" and record["transport"] == "virtual"
                                 and record["wire_timestamp_ns"] == 0 for record in traffic), 1)

    def test_broadcast_backend_exception_is_visible_after_internal_delivery(self):
        tv, avr, player = self.topology()
        with CecSession([tv, avr, player], backend_factory=self.backend) as session:
            self.clear_wire()

            def broken_wire(frame):
                self.backends[player.gpio].sent.append(frame)
                raise OSError("injected disconnected wire")

            self.backends[player.gpio].transmit = broken_wire
            with self.assertRaisesRegex(OSError, "injected disconnected wire"):
                player.send(15, 0x82, player.address.physical.to_bytes(2, "big"))
            self.assertEqual(avr.active_input, player.address.physical)
            self.assertEqual(len(self.backends[player.gpio].sent), 1)
            traffic = [record for record in session.records if record["opcode"] == 0x82]
            wire = [record for record in traffic if record["direction"] == "tx" and record["transport"] == "wire"]
            self.assertEqual(len(wire), 1)
            self.assertFalse(wire[0]["ok"])
            self.assertIn("injected disconnected wire", wire[0]["error"])
            self.assertEqual(sum(record["direction"] == "rx" and record["transport"] == "virtual"
                                 for record in traffic), 1)
            session.assert_healthy()

    def test_before_tx_destination_rewrite_changes_virtual_or_wire_routing(self):
        tv, avr, player = self.topology()
        with CecSession([tv, avr, player], backend_factory=self.backend):
            self.clear_wire()
            player.hooks.before_tx = lambda component, frame: Frame(frame.source, avr.address.logical,
                                                                    frame.opcode, frame.operands)
            virtual = player.send(0, 0x44, [0x41])  # Original destination is the external DUT.
            self.assertEqual(virtual.transport, "virtual")
            self.assertEqual(avr.volume, 26)
            self.assert_no_wire_tx()

            player.hooks.before_tx = lambda component, frame: Frame(frame.source, 0,
                                                                    frame.opcode, frame.operands)
            physical = player.send(avr.address.logical, 0x44, [0x41])
            self.assertEqual(physical.transport, "wire")
            self.assertEqual(avr.volume, 26)
            self.assertEqual([frame.destination for frame in self.backends[player.gpio].sent], [0])
            self.assertFalse(self.backends[tv.gpio].sent)

    def test_recursive_virtual_responses_fail_boundedly_and_unwind_cleanly(self):
        tv, avr, player = self.topology()
        with CecSession([tv, avr, player], backend_factory=self.backend) as session:
            self.clear_wire()
            original_avr, original_player = avr.handle, player.handle
            avr.handle = lambda frame: avr.send(frame.source, 0x89)
            player.handle = lambda frame: player.send(frame.source, 0x89)
            baseline = len(session.records)
            try:
                with self.assertRaisesRegex(CecError, "response loop"):
                    player.send(avr.address.logical, 0x89)
            finally:
                avr.handle, player.handle = original_avr, original_player
            traffic = session.records[baseline:]
            self.assertGreater(len(traffic), 2)
            self.assertLess(len(traffic), 200)
            self.assertTrue(all(record["transport"] == "virtual" for record in traffic))
            self.assert_no_wire_tx()
            # A bounded fault must unwind all delivery depth and ownership.
            session.assert_healthy()
            self.assertTrue(player.send(avr.address.logical, 0x44, [0x41]).ok)
            self.assertEqual(avr.volume, 26)

    def test_virtual_tx_fault_drop_suppresses_peer_and_wire(self):
        tv, avr, player = self.topology()
        with CecSession([tv, avr, player], backend_factory=self.backend):
            self.clear_wire()
            player.hooks.before_tx = lambda component, frame: None
            self.assertIsNone(player.send(avr.address.logical, 0x44, [0x41]))
            self.assertEqual(avr.volume, 25)
            self.assert_no_wire_tx()

    def test_virtual_rx_fault_drop_suppresses_handler_but_preserves_ack(self):
        tv, avr, player = self.topology()
        with CecSession([tv, avr, player], backend_factory=self.backend) as session:
            self.clear_wire()
            avr.hooks.before_rx = lambda component, frame: None
            result = player.send(avr.address.logical, 0x44, [0x41])
            self.assertTrue(result.ok)
            self.assertEqual(result.transport, "virtual")
            self.assertEqual(avr.volume, 25)
            self.assert_no_wire_tx()
            dropped = [record for record in session.records if record.get("dropped")]
            self.assertEqual(len(dropped), 1)
            self.assertEqual(dropped[0]["transport"], "virtual")
            self.assertEqual(dropped[0]["wire_timestamp_ns"], 0)

    def test_off_sender_cannot_use_virtual_bus(self):
        tv, avr, player = self.topology()
        with CecSession([tv, avr, player], backend_factory=self.backend):
            player.power_off()
            self.clear_wire()
            with self.assertRaises(CecError):
                player.send(avr.address.logical, 0x44, [0x41])
            self.assertEqual(avr.volume, 25)
            self.assert_no_wire_tx()

    def test_self_poll_nacks_and_self_directed_never_loops_back(self):
        tv, avr, player = self.topology()
        with CecSession([tv, avr, player], backend_factory=self.backend):
            self.clear_wire()
            poll = player.send(player.address.logical)
            self.assertFalse(poll.ok)
            self.assertEqual(poll.transport, "virtual")
            try:
                directed = player.send(player.address.logical, 0x44, [0x44])
            except CecError:
                pass  # Explicit rejection is also a valid self-send policy.
            else:
                self.assertFalse(directed.ok)
                self.assertEqual(directed.transport, "virtual")
            self.assertFalse(player.playing)
            self.assert_no_wire_tx()

    def test_close_releases_wire_and_prevents_stale_component_sends(self):
        tv, avr, player = self.topology()
        with CecSession([tv, avr, player], backend_factory=self.backend):
            pass
        self.assertTrue(all(backend.closed and not backend.claimed for backend in self.backends.values()))
        self.assertTrue(all(component._session is None for component in (tv, avr, player)))
        self.clear_wire()
        with self.assertRaises(CecError):
            player.send(avr.address.logical, 0x44, [0x41])
        self.assert_no_wire_tx()

    def test_foreign_component_cannot_submit_into_session(self):
        tv, avr, player = self.topology()
        foreign = PlaybackComponent(self.dut, "Foreign Player", address=CecAddress(8, "2.0.0.0"))
        with CecSession([tv, avr, player], backend_factory=self.backend) as session:
            self.clear_wire()
            with self.assertRaises(CecError):
                session.transmit(foreign, Frame(8, avr.address.logical, 0x44, bytes([0x41])))
            self.assertEqual(avr.volume, 25)
            self.assert_no_wire_tx()


if __name__ == "__main__":
    unittest.main()
