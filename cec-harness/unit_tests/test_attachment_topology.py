"""Address assignment follows physical attachments without bridging CEC wires."""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import AvrComponent, CecAddress, DUT, PlaybackComponent, TvComponent, assign_addresses


class AttachmentTopologyTests(unittest.TestCase):
    def tree(self, dut=None, *, tentacle=0, tv_input=1):
        dut = dut or DUT("amity-dut")
        connector = dut.tentacle(tentacle, tv_input=tv_input)
        tv = TvComponent(connector.front, "Topology TV")
        avr = AvrComponent(connector.back, "Topology AVR")
        player = PlaybackComponent(avr, "Topo Player", port=1)
        return tv, avr, player

    def test_inline_dut_uses_tv_input_without_an_extra_address_level(self):
        tv, avr, player = self.tree(tv_input=3)
        assign_addresses([player, avr, tv])
        self.assertEqual(tv.address, CecAddress(0, "0.0.0.0"))
        self.assertEqual(avr.address, CecAddress(5, "3.0.0.0"))
        self.assertEqual(player.address, CecAddress(4, "3.1.0.0"))
        self.assertIsNone(avr.parent)
        self.assertIs(player.parent, avr)
        self.assertNotEqual(tv.gpio, avr.gpio)
        self.assertEqual(avr.gpio, player.gpio)
        self.assertEqual((avr.direction, player.direction), ("back", "back"))

    def test_paired_root_explicit_address_must_match_the_tv_input(self):
        tv, avr, player = self.tree(tv_input=3)
        avr.address = CecAddress(5, "2.0.0.0")
        with self.assertRaisesRegex(ValueError, "inline DUT TV input"):
            assign_addresses([player, tv, avr])
        self.assertIsNone(tv.address)
        self.assertIsNone(player.address)

        avr.address = CecAddress(5, "3.0.0.0")
        assign_addresses([tv, avr, player])
        self.assertEqual(player.address.physical, 0x3100)

    def test_standalone_back_root_allows_an_explicit_base_without_a_tv(self):
        dut = DUT("amity-dut")
        avr = AvrComponent(dut.tentacle(0).back, "Standalone AVR", address=CecAddress(5, "2.0.0.0"))
        player = PlaybackComponent(avr, "Stand Player", port=3)
        assign_addresses([player, avr])
        self.assertEqual(avr.address.physical, 0x2000)
        self.assertEqual(player.address.physical, 0x2300)
        unbased = AvrComponent(dut.tentacle(0).back, "No Base AVR")
        with self.assertRaisesRegex(ValueError, "explicit physical address"):
            assign_addresses([unbased])

    def test_different_duts_and_adapters_have_independent_address_domains(self):
        multi = DUT("amity-dut-4")
        trees = [self.tree(multi, tentacle=0), self.tree(multi, tentacle=1, tv_input=3),
                 self.tree(DUT("amity-dut-2"))]
        components = [component for tree in trees for component in tree]
        assign_addresses(list(reversed(components)))
        for tree, base in zip(trees, (0x1000, 0x3000, 0x1000)):
            tv, avr, player = tree
            self.assertEqual((tv.address.logical, avr.address.logical, player.address.logical), (0, 5, 4))
            self.assertEqual((tv.address.physical, avr.address.physical, player.address.physical),
                             (0, base, base | 0x100))
        self.assertEqual(len({component.gpio for component in components}), 6)

    def test_paired_endpoints_cannot_disagree_about_the_tv_input(self):
        dut = DUT("amity-dut")
        tv = TvComponent(dut.tentacle(0, tv_input=1).front, "Input One TV")
        avr = AvrComponent(dut.tentacle(0, tv_input=3).back, "Input3 AVR")
        with self.assertRaisesRegex(ValueError, "disagree on the TV input"):
            assign_addresses([avr, tv])
        self.assertIsNone(tv.address)
        self.assertIsNone(avr.address)

    def test_corrupted_crosswire_component_parent_is_rejected(self):
        for other_dut in (False, True):
            with self.subTest(other_dut=other_dut):
                tv, avr, player = self.tree()
                if other_dut:
                    foreign_tv, foreign_avr, _ = self.tree(DUT("amity-dut-2"))
                    components = [tv, avr, player, foreign_tv, foreign_avr]
                    player._attachment = foreign_avr
                else:
                    components = [tv, avr, player]
                    player._attachment = tv
                with self.assertRaisesRegex(ValueError, "same isolated CEC wire"):
                    assign_addresses(components)
                self.assertTrue(all(component.address is None for component in components))

    def test_corrupted_cycle_is_rejected_without_partial_assignment(self):
        tv, avr, player = self.tree()
        avr._attachment = player
        with self.assertRaisesRegex(ValueError, "cycle"):
            assign_addresses([tv, avr, player])
        self.assertTrue(all(component.address is None for component in (tv, avr, player)))

    def test_physical_collisions_include_both_sides_of_the_inline_dut(self):
        tv, avr, player = self.tree()
        front_source = PlaybackComponent(tv, "Front Source", port=1)
        with self.assertRaisesRegex(ValueError, "same HDMI physical address"):
            assign_addresses([tv, avr, player, front_source])
        self.assertTrue(all(component.address is None for component in (tv, avr, player, front_source)))

    def test_logical_address_exhaustion_does_not_partially_assign_other_components(self):
        tv, avr, player = self.tree()
        with self.assertRaisesRegex(ValueError, "No logical address"):
            assign_addresses([tv, avr, player], reserved={player.gpio: {4, 8, 11}})
        self.assertTrue(all(component.address is None for component in (tv, avr, player)))


if __name__ == "__main__":
    unittest.main()
