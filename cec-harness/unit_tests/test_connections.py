"""Attachment structure and Markdown GPIO resolution; no physical operation."""

from dataclasses import FrozenInstanceError
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import (AvrComponent, DUT, PlaybackComponent, Tentacle,
                           TentacleEndpoint, TvComponent)
from unit_tests.harness_pins import cec_gpio, wiring


class ConnectionTests(unittest.TestCase):
    def test_named_dut_tentacle_resolves_documented_paired_gpios_without_io(self):
        with patch("amity_harness.dut.PinctrlGPIO.low") as low, \
                patch("amity_harness.dut.PinctrlGPIO.release") as release, \
                patch("amity_harness.dut.DUT.request") as request:
            dut = DUT("amity-dut")
            tentacle = dut.tentacle(0)
            self.assertIsInstance(tentacle, Tentacle)
            self.assertIs(tentacle.front.dut, dut)
            self.assertEqual((tentacle.front.tentacle, tentacle.front.direction, tentacle.front.gpio), (0, "front", cec_gpio(dut.name, direction="front")))
            self.assertEqual((tentacle.back.tentacle, tentacle.back.direction, tentacle.back.gpio), (0, "back", cec_gpio(dut.name)))
            self.assertEqual((tentacle.front.tv_input, tentacle.back.tv_input), (1, 1))
            self.assertFalse(low.called or release.called or request.called)

    def test_all_documented_adapters_and_configurable_upstream_tv_input(self):
        dut = DUT("amity-dut-4")
        indices = sorted({index for name, index, _ in wiring().cec if name == dut.name})
        for index in indices:
            front_gpio = cec_gpio(dut.name, index, "front")
            back_gpio = cec_gpio(dut.name, index, "back")
            with self.subTest(index=index):
                adapter = dut.tentacle(index, tv_input=3)
                self.assertEqual((adapter.front.gpio, adapter.back.gpio), (front_gpio, back_gpio))
                self.assertEqual(adapter.front.tv_input, 3)
                self.assertEqual(adapter.back.tv_input, 3)
        with self.assertRaises(ValueError):
            DUT("amity-dut").tentacle(1)
        for bad in (-1, True, 0.0, "0"):
            with self.subTest(index=bad), self.assertRaises(ValueError):
                dut.tentacle(bad)
        for bad in (0, 16, True, 1.0, "1"):
            with self.subTest(port=bad), self.assertRaises(ValueError):
                dut.tentacle(0, tv_input=bad)

    def test_endpoints_are_immutable_and_equal_only_for_same_dut_and_connection(self):
        dut = DUT("amity-dut")
        endpoint = dut.tentacle(0).front
        self.assertEqual(endpoint, dut.tentacle(0).front)
        self.assertEqual(hash(endpoint), hash(dut.tentacle(0).front))
        self.assertNotEqual(endpoint, dut.tentacle(0).back)
        self.assertNotEqual(endpoint, dut.tentacle(0, tv_input=2).front)
        self.assertNotEqual(endpoint, DUT("amity-dut").tentacle(0).front)
        for field, value in (("gpio", cec_gpio(dut.name)), ("direction", "back"), ("tv_input", 2)):
            with self.subTest(field=field), self.assertRaises(FrozenInstanceError):
                setattr(endpoint, field, value)

    def test_real_test_topology_attaches_tv_and_avr_to_dut_and_player_to_avr(self):
        dut = DUT("amity-dut")
        tentacle = dut.tentacle(0, tv_input=2)
        tv = TvComponent(tentacle.front, "Unit TV")
        avr = AvrComponent(tentacle.back, "Unit AVR")
        player = PlaybackComponent(avr, "Unit Player", port=1)
        self.assertIs(tv.attachment, tentacle.front)
        self.assertIs(avr.attachment, tentacle.back)
        self.assertIsNone(tv.parent)
        self.assertIsNone(avr.parent)
        self.assertIs(player.attachment, avr)
        self.assertIs(player.parent, avr)
        self.assertIs(player.endpoint, tentacle.back)
        self.assertEqual((player.dut, player.tentacle, player.direction, player.gpio, player.tv_input),
                         (dut, 0, "back", cec_gpio(dut.name), 2))
        self.assertEqual(player.port, 1)
        self.assertEqual(tv.gpio, cec_gpio(dut.name, direction="front"))

    def test_direct_dut_convenience_is_normalized_and_tv_default_does_not_override_endpoint(self):
        dut = DUT("amity-dut")
        tv = TvComponent(dut, "Unit TV")
        player = PlaybackComponent(dut, "Unit Player")
        self.assertIsInstance(tv.attachment, TentacleEndpoint)
        self.assertEqual((tv.direction, tv.gpio), ("front", cec_gpio(dut.name, direction="front")))
        self.assertEqual((player.direction, player.gpio), ("back", cec_gpio(dut.name)))
        back_tv = TvComponent(dut.tentacle(0).back, "Back TV")
        front_player = PlaybackComponent(dut.tentacle(0).front, "Front Player")
        self.assertEqual((back_tv.direction, back_tv.gpio), ("back", cec_gpio(dut.name)))
        self.assertEqual((front_player.direction, front_player.gpio), ("front", cec_gpio(dut.name, direction="front")))
        keyword_tv = TvComponent(attachment=dut, name="Keyword TV")
        self.assertEqual(keyword_tv.direction, "front")

    def test_duck_typed_dut_uses_existing_lookup_without_hardware_methods(self):
        lookups = []

        documented_gpio = cec_gpio("amity-dut-4", 2, "back")

        class Wiring:
            def cec_gpio(self, name, tentacle, direction):
                lookups.append((name, tentacle, direction))
                return documented_gpio

        dut = SimpleNamespace(name="unit-dut", wiring=Wiring())
        avr = AvrComponent(dut, "Unit AVR", tentacle=2)
        player = PlaybackComponent(avr, "Unit Player", port=1)
        self.assertEqual(lookups, [("unit-dut", 2, "back")])
        self.assertEqual(hash(avr.endpoint), hash(player.endpoint))
        self.assertIs(player.dut, dut)
        self.assertEqual(player.gpio, documented_gpio)

    def test_conflicting_overrides_and_parallel_parent_keyword_are_rejected(self):
        dut = DUT("amity-dut")
        avr = AvrComponent(dut.tentacle(0).back, "Unit AVR")
        for attachment, kwargs in ((avr, {"port": 1, "direction": "front"}),
                                   (avr, {"port": 1, "tentacle": 1}),
                                   (avr, {"port": 1, "tentacle": 0.0}),
                                   (avr.endpoint, {"direction": "front"}),
                                   (avr.endpoint, {"tentacle": True}),
                                   (avr.endpoint, {"port": 1}),
                                   (dut, {"port": 1})):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                PlaybackComponent(attachment, "Bad Player", **kwargs)
        for port in (None, 0, 16, True, 1.0):
            with self.subTest(port=port), self.assertRaises(ValueError):
                PlaybackComponent(avr, "Bad Player", port=port)
        with self.assertRaises(TypeError):
            PlaybackComponent(dut, "Bad Player", parent=avr, port=1)
        child = PlaybackComponent(avr, "Unit Player", port=1, tentacle=0, direction="back")
        for field, value in (("dut", dut), ("direction", "front"), ("gpio", cec_gpio(dut.name, direction="front")),
                             ("tentacle", 1), ("parent", None), ("attachment", dut)):
            with self.subTest(field=field), self.assertRaises(AttributeError):
                setattr(child, field, value)


if __name__ == "__main__":
    unittest.main()
