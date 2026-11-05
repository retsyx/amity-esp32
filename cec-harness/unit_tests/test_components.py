"""Framework unit checks; no DUT, radio, network or GPIO operation."""

import ctypes
import unittest

from amity_harness import DUT
from amity_harness.cec import (CecError, CecSession, Frame, TxResult, _Caps,
                               _LogAddrs, _Message, _request)
from amity_harness.components import (AvrComponent, Component, Hooks,
                                     PlaybackComponent, PowerState, TvComponent)
from amity_harness.topology import (CecAddress, assign_addresses, child_address,
                                   physical_address)


def topology():
    dut = DUT("amity-dut")
    tv = TvComponent(dut.tentacle(0).front, "Unit TV")
    avr = AvrComponent(dut.tentacle(0).back, "Unit AVR")
    playback = PlaybackComponent(avr, "Unit Player", port=2)
    return tv, avr, playback


class FakeBackend:
    def __init__(self, gpio):
        self.gpio = gpio
        self.configurations = []
        self.messages = []
        self.closed = False

    def open(self):
        return self

    def configure(self, components):
        self.configurations.append(tuple(c.address.logical for c in components))

    def transmit(self, frame):
        self.messages.append(frame)
        return TxResult(frame, 1)

    def receive(self, timeout):
        # No mock event source: protocol tests inject received frames directly.
        import time
        time.sleep(min(timeout, 0.001))
        return None

    def close(self):
        self.closed = True


class FrameworkComponentTests(unittest.TestCase):
    def test_explicit_mute_commands_set_state_without_toggling_on_repetition(self):
        tv, avr, player = topology()
        with CecSession([tv, avr, player], backend_factory=FakeBackend):
            for component, initiator in ((tv, 4), (avr, 0)):
                with self.subTest(component=component.name):
                    for key, muted in ((0x65, True), (0x65, True), (0x66, False), (0x66, False)):
                        component.receive(Frame(initiator, component.address.logical, 0x44, bytes([key])))
                        component.receive(Frame(initiator, component.address.logical, 0x45))
                        self.assertEqual(component.muted, muted)

    def test_linux_abi_sizes_and_requests(self):
        self.assertEqual(ctypes.sizeof(_Caps), 76)
        self.assertEqual(ctypes.sizeof(_LogAddrs), 92)
        self.assertEqual(ctypes.sizeof(_Message), 56)
        self.assertEqual(_request(3, 0, _Caps), 0xc04c6100)
        self.assertEqual(_request(3, 4, _LogAddrs), 0xc05c6104)
        self.assertEqual(_request(3, 5, _Message), 0xc0386105)

    def test_topology_assigns_addresses_by_type_and_ports(self):
        tv, avr, playback = topology()
        assign_addresses([playback, tv, avr])  # independent of input ordering
        self.assertEqual(tv.address, CecAddress(0, "0.0.0.0"))
        self.assertEqual(avr.address, CecAddress(5, "1.0.0.0"))
        self.assertEqual(playback.address, CecAddress(4, "1.2.0.0"))

    def test_invalid_and_colliding_topologies_fail_before_io(self):
        tv, avr, playback = topology()
        playback.port = 0
        with self.assertRaises(ValueError):
            assign_addresses([tv, avr, playback])
        playback.port = 1
        avr._attachment = playback
        with self.assertRaisesRegex(ValueError, "cycle"):
            assign_addresses([tv, avr, playback])
        tv, avr, playback = topology()
        playback.address = CecAddress(4, "1.3.0.0")
        with self.assertRaisesRegex(ValueError, "disagrees"):
            assign_addresses([tv, avr, playback])
        for bad in ("1.0.2.0", "1.2.3", 0xffff, -1):
            with self.assertRaises(ValueError):
                physical_address(bad)
        with self.assertRaises(ValueError):
            child_address("1.2.3.4", 1)

    def test_exact_discovery_reply_vectors_and_per_component_identity(self):
        tv, avr, playback = topology()
        with CecSession([tv, avr, playback], backend_factory=FakeBackend) as session:
            back = session._backends[avr.gpio]
            back.messages.clear()
            avr.receive(Frame.from_bytes(bytes.fromhex("0583")))
            playback.receive(Frame.from_bytes(bytes.fromhex("0483")))
            avr.receive(Frame.from_bytes(bytes.fromhex("0546")))
            playback.receive(Frame.from_bytes(bytes.fromhex("0446")))
            self.assertEqual([message.raw for message in back.messages], [
                bytes.fromhex("5f84100005"), bytes.fromhex("4f84120004"),
                b"\x50\x47Unit AVR", b"\x40\x47Unit Player"])
            self.assertTrue(all(record["ok"] for record in session.records if record["direction"] == "tx"))

    def test_off_removes_address_and_blocks_all_component_io(self):
        tv, avr, playback = topology()
        observed = []
        playback.hooks = Hooks(before_rx=lambda c, frame: observed.append(frame) or frame)
        with CecSession([tv, avr, playback], backend_factory=FakeBackend) as session:
            backend = session._backends[avr.gpio]
            observed.clear()  # Ignore peer announcements while initially powered.
            playback.power_off()
            self.assertEqual(backend.configurations[-1], (5,))
            count = len(backend.messages)
            playback.receive(Frame(0, 4, 0x8f))
            with self.assertRaises(CecError):
                playback.send(0, 0x90, [1])
            self.assertEqual(len(backend.messages), count)
            self.assertEqual(observed, [])
            playback.standby()
            self.assertEqual(backend.configurations[-1], (5, 4))
            playback.receive(Frame(0, 4, 0x8f))
            self.assertEqual(backend.messages[-1].raw, bytes.fromhex("409001"))
            playback.power_on()
            playback.receive(Frame(0, 4, 0x8f))
            self.assertEqual(backend.messages[-1].raw, bytes.fromhex("409000"))
        self.assertTrue(backend.closed)
        self.assertIsNone(playback._session)

    def test_hooks_drop_receive_and_transmit_and_monitor_results(self):
        tv, avr, playback = topology()
        seen = []
        with CecSession([tv, avr, playback], backend_factory=FakeBackend) as session:
            backend = session._backends[avr.gpio]
            backend.messages.clear()
            avr.hooks = Hooks(before_rx=lambda c, frame: None)
            avr.receive(Frame(0, 5, 0x46))
            self.assertEqual(backend.messages, [])
            self.assertTrue(session.records[-1]["dropped"])
            avr.hooks = Hooks(before_tx=lambda c, frame: None)
            avr.receive(Frame(0, 5, 0x46))
            self.assertEqual(backend.messages, [])
            avr.hooks = Hooks(after_tx=lambda c, result: seen.append(result.ok),
                              after_rx=lambda c, frame: seen.append(frame.opcode))
            avr.receive(Frame(0, 5, 0x8f))
            self.assertEqual(seen, [True, 0x8f])

    def test_tv_avr_and_player_state_changes(self):
        tv, avr, playback = topology()
        with CecSession([tv, avr, playback], backend_factory=FakeBackend):
            tv.receive(Frame(4, 0, 0x04))
            self.assertEqual(tv.power, PowerState.ON)
            avr.receive(Frame(0, 5, 0x70, b"\x12\x00"))
            avr.receive(Frame(0, 5, 0x44, b"\x41"))
            self.assertTrue(avr.system_audio)
            self.assertEqual(avr.volume, 26)
            playback.receive(Frame(0, 15, 0x86, b"\x12\x00"))
            self.assertTrue(playback.active)
            self.assertEqual(playback.power, PowerState.ON)
            playback.receive(Frame(0, 15, 0x36))
            self.assertFalse(playback.active)
            self.assertEqual(playback.power, PowerState.STANDBY)

    def test_tv_volume_mute_and_release_follow_directed_cec_keys_without_an_avr(self):
        dut = DUT("amity-dut")
        tv = TvComponent(dut.tentacle(0, tv_input=3).front, "TV audio")
        player = PlaybackComponent(dut.tentacle(0, tv_input=3).back, "Direct player")
        with CecSession((tv, player), backend_factory=FakeBackend):
            self.assertEqual((tv.volume, tv.muted), (25, False))
            tv.receive(Frame(4, tv.address.logical, 0x44, b"\x41"))
            self.assertEqual((tv.volume, tv.pressed_key), (26, 0x41))
            tv.receive(Frame(4, tv.address.logical, 0x45))
            self.assertIsNone(tv.pressed_key)
            self.assertEqual(tv.volume, 26)
            tv.receive(Frame(4, tv.address.logical, 0x44, b"\x42"))
            self.assertEqual(tv.volume, 25)
            for muted in (True, False):
                tv.receive(Frame(4, tv.address.logical, 0x44, b"\x43"))
                tv.receive(Frame(4, tv.address.logical, 0x45))
                self.assertEqual(tv.muted, muted)
            for value, key in ((100, b"\x41"), (0, b"\x42")):
                tv.volume = value
                tv.receive(Frame(4, tv.address.logical, 0x44, key))
                self.assertEqual(tv.volume, value)
            tv.receive(Frame(4, 5, 0x44, b"\x41"))
            tv.receive(Frame(4, 15, 0x44, b"\x41"))
            self.assertEqual(tv.volume, 0)

    def test_trace_is_bounded_and_snapshots_cannot_mutate_internal_records(self):
        tv, avr, playback = topology()
        with CecSession([tv, avr, playback], backend_factory=FakeBackend, max_records=3) as session:
            snapshot = session.records
            self.assertEqual(len(snapshot), 3)
            snapshot[-1]["component"] = "tampered"
            self.assertNotEqual(session.records[-1]["component"], "tampered")


if __name__ == "__main__":
    unittest.main()
