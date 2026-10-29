"""Explicit allocation profiles retain playback behavior and wire identity."""

from pathlib import Path
import sys
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import (AvrComponent, CecAddress, CecSession, DUT, Frame,
                           PlaybackComponent, TvComponent, assign_addresses)
from test_component_routing_assertions import IdleWire


class Cec14Wire(IdleWire):
    cec_version = 5


class PlaybackProfileTests(unittest.TestCase):
    def test_two_players_have_distinct_avr_inputs_and_allocation_slots(self):
        dut = DUT("amity-dut")
        tentacle = dut.tentacle(0)
        tv = TvComponent(tentacle.front, "Profile TV")
        avr = AvrComponent(tentacle.back, "Profile AVR")
        player = PlaybackComponent(avr, "First Player", port=1)
        other = PlaybackComponent(avr, "Second Player", port=2, address_type="tuner")
        assign_addresses([tv, avr, player, other])
        self.assertEqual([c.address.logical for c in (avr, player, other)], [5, 4, 3])
        self.assertEqual([c.address.physical for c in (avr, player, other)], [0x1000, 0x1100, 0x1200])
        self.assertEqual(other.logical_address_type, 2)
        self.assertEqual(other.device_type, 4)
        self.assertEqual(other.gpio, avr.gpio)
        self.assertEqual(other.address_type, "tuner")

    def test_recording_pool_is_explicit_and_invalid_profiles_or_addresses_fail(self):
        endpoint = DUT("amity-dut").tentacle(0).back
        player = PlaybackComponent(endpoint, "Recorder", address_type="recording",
                                   address=CecAddress(1, 0x1000))
        assign_addresses([player])
        self.assertEqual(player.logical_address_type, 1)
        self.assertEqual(player.device_type, 4)
        for profile in (None, True, "unknown"):
            with self.subTest(profile=profile), self.assertRaises(ValueError):
                PlaybackComponent(endpoint, "Invalid", address_type=profile)
        with self.assertRaises(ValueError):
            assign_addresses([PlaybackComponent(endpoint, "Invalid", address_type="tuner",
                                               address=CecAddress(8, 0x1000))])

    def test_cec14_claims_advertise_cec14_and_do_not_send_cec20_features(self):
        player = PlaybackComponent(DUT("amity-dut").tentacle(0).back, "CEC14 Player",
                                   address=CecAddress(4, 0x1000))
        with CecSession([player], backend_factory=Cec14Wire) as session:
            self.assertEqual(player.cec_version, 5)
            self.assertFalse(any(r["opcode"] == 0xa6 for r in session.records))
            player.receive(Frame(0, 4, 0x9f, timestamp_ns=time.monotonic_ns()))
            self.assertTrue(any(r["direction"] == "tx" and r["frame"] == "40:9e:05"
                                for r in session.records))
            player.receive(Frame(0, 4, 0xa5, timestamp_ns=time.monotonic_ns()))
            self.assertTrue(any(r["direction"] == "tx" and r["frame"] == "40:00:a5:00"
                                for r in session.records))
            player.activate()
            self.assertTrue(player.active)


if __name__ == "__main__":
    unittest.main()
