"""Verify wiring-derived CEC boot configuration without touching a Pi."""

from pathlib import Path
import shutil
import subprocess
import tempfile
from types import SimpleNamespace
import unittest

from amity_harness.pi_boot import boot_config, overlay_source
from amity_harness.wiring import Wiring


class PiBootTests(unittest.TestCase):
    def setUp(self):
        self.wiring = Wiring.from_markdown()

    def test_every_documented_cec_wire_gets_one_open_drain_adapter(self):
        source = overlay_source(self.wiring)
        self.assertEqual(source.count('compatible = "cec-gpio";'), len(self.wiring.cec))
        for pin in self.wiring.cec.values():
            self.assertIn(f"cec@{pin} {{", source)
            self.assertIn(f"cec-gpios = <&rp1_gpio {pin} 6>;", source)
            self.assertIn(f'pins = "gpio{pin}";', source)
            self.assertIn(f"pinctrl-0 = <&amity_cec_{pin}>;", source)
        self.assertEqual(source.count("bias-disable;"), len(self.wiring.cec))
        for pin in self.wiring.reset.values():
            self.assertNotIn(f"cec@{pin} {{", source)

    def test_config_is_idempotent_preserves_settings_and_migrates_old_cec_lines(self):
        original = ("# keep\n[pi5]\ndtparam=audio=on\n"
                    "dtoverlay=cec-gpio,gpio_pin=23\n"
                    "  dtoverlay = cec-gpio,gpio_pin=24 # old\n"
                    "# dtoverlay=cec-gpio,gpio_pin=25\n"
                    "dtoverlay=cec-gpio-other\n[none]\n")
        configured = boot_config(original, self.wiring)
        self.assertEqual(boot_config(configured, self.wiring), configured)
        self.assertIn("# keep\n[pi5]\ndtparam=audio=on\n", configured)
        self.assertIn("# dtoverlay=cec-gpio,gpio_pin=25\n", configured)
        self.assertIn("dtoverlay=cec-gpio-other\n[none]\n", configured)
        self.assertNotIn("gpio_pin=23", configured)
        self.assertNotIn("gpio_pin=24", configured)
        self.assertEqual(configured.count("dtoverlay=amity-harness\n"), 1)
        pins = sorted([*self.wiring.cec.values(), *self.wiring.reset.values()])
        self.assertIn("gpio=" + ",".join(map(str, pins)) + "=ip,np\n", configured)
        self.assertIn("# BEGIN Amity harness\n[all]\n", configured)

    def test_replacing_managed_block_picks_up_wiring_changes(self):
        before = SimpleNamespace(cec={"wire": 23}, reset={"dut": 4})
        after = SimpleNamespace(cec={"wire": 24}, reset={"dut": 11})
        configured = boot_config(boot_config("", before), after)
        self.assertIn("gpio=11,24=ip,np\n", configured)
        self.assertNotIn("gpio=4,23", configured)

    def test_invalid_wiring_is_rejected_before_generating_settings(self):
        for cec, reset in (([28], [4]), ([-1], [4]), ([True], [4]), ([23], [23]),
                           ([23, 23], [4]), ([], [4]), ([23], [])):
            with self.subTest(cec=cec, reset=reset):
                wiring = SimpleNamespace(cec=dict(enumerate(cec)), reset=dict(enumerate(reset)))
                with self.assertRaises(ValueError):
                    overlay_source(wiring)
                with self.assertRaises(ValueError):
                    boot_config("", wiring)

    def test_malformed_managed_blocks_do_not_truncate_user_settings(self):
        for document in ("# BEGIN Amity harness\nkeep\n", "# END Amity harness\n",
                         "# BEGIN Amity harness\n# BEGIN Amity harness\n# END Amity harness\n"):
            with self.subTest(document=document), self.assertRaises(ValueError):
                boot_config(document, self.wiring)

    @unittest.skipUnless(shutil.which("dtc") and shutil.which("fdtoverlay"), "device-tree tools unavailable")
    def test_compiled_overlay_applies_to_rp1_and_retains_decimal_node_names(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "base.dts").write_text('''/dts-v1/;
/ { compatible = "raspberrypi,5-model-b";
    rp1_gpio: gpio { gpio-controller; #gpio-cells = <2>; };
};
''')
            (root / "overlay.dts").write_text(overlay_source(self.wiring))
            for name in ("base", "overlay"):
                subprocess.run(["dtc", "-q", "-@", "-I", "dts", "-O", "dtb", "-o",
                                str(root / (name + ".dtb")), str(root / (name + ".dts"))],
                               check=True, capture_output=True)
            subprocess.run(["fdtoverlay", "-i", str(root / "base.dtb"), "-o", str(root / "merged.dtb"),
                            str(root / "overlay.dtb")], check=True, capture_output=True)
            merged = subprocess.run(["dtc", "-q", "-I", "dtb", "-O", "dts", str(root / "merged.dtb")],
                                    check=True, capture_output=True, text=True).stdout
            for pin in self.wiring.cec.values():
                self.assertIn(f"cec@{pin} {{", merged)
                self.assertIn(f'"gpio{pin}"', merged)


if __name__ == "__main__":
    unittest.main()
