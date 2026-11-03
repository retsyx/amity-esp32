"""Missing GPIO CEC support never replaces or changes the stock CEC core."""

from io import BytesIO
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import pi_cec_driver as driver


class DriverTests(unittest.TestCase):
    def runner(self, *, existing=False, headers=True, status=""):
        calls = []

        def run(command, **kwargs):
            calls.append(command)
            code = (0 if existing else 1) if command[0] == "modinfo" else 0
            if command[0] == "apt-cache":
                code = 0 if headers else 100
            return subprocess.CompletedProcess(command, code, status if command[:2] == ["dkms", "status"] else "", "")

        return run, calls

    def headers(self, directory, *, configuration="CONFIG_CEC_CORE=m\n", symbols="0xabc\tcec_allocate_adapter\tcec\tEXPORT_SYMBOL_GPL\n"):
        build = Path(directory) / "6.12.1+rpt-rpi-2712" / "build"
        build.mkdir(parents=True)
        (build / "Makefile").touch()
        (build / ".config").write_text(configuration)
        (build / "Module.symvers").write_text(symbols)

    def test_existing_driver_is_loaded_without_installing_anything(self):
        run, calls = self.runner(existing=True)
        with patch.object(driver, "_install_sources") as install:
            self.assertFalse(driver.ensure_driver(run))
        self.assertEqual(calls, [["modinfo", "cec-gpio"], ["modprobe", "cec-gpio"]])
        install.assert_not_called()

    def test_unsupported_kernel_refuses_before_apt_or_dkms_changes(self):
        run, calls = self.runner()
        with patch.object(driver.platform, "release", return_value="7.0.0-rpi"):
            with self.assertRaisesRegex(RuntimeError, "supported kernel series"):
                driver.ensure_driver(run)
        self.assertEqual(calls, [["modinfo", "cec-gpio"]])

    def test_aged_out_headers_install_matching_image_and_request_reboot(self):
        run, calls = self.runner(headers=False)
        with tempfile.TemporaryDirectory() as directory, \
                patch.object(driver, "_MODULE_ROOT", Path(directory)), \
                patch.object(driver.platform, "release", return_value="6.12.1+rpt-rpi-2712"), \
                patch.object(driver, "_install_sources") as install:
            self.assertTrue(driver.ensure_driver(run))
        install.assert_not_called()
        self.assertIn("linux-image-rpi-2712", calls[-1])
        self.assertIn("linux-headers-rpi-2712", calls[-1])
        self.assertFalse(any(command[0] == "dkms" for command in calls))

    def test_core_disabled_refuses_before_module_install(self):
        run, calls = self.runner()
        with tempfile.TemporaryDirectory() as directory:
            self.headers(directory, configuration="# CONFIG_CEC_CORE is not set\n")
            with patch.object(driver, "_MODULE_ROOT", Path(directory)), \
                    patch.object(driver.platform, "release", return_value="6.12.1+rpt-rpi-2712"), \
                    patch.object(driver, "_install_sources") as install:
                with self.assertRaisesRegex(RuntimeError, "lacks the stock CEC core"):
                    driver.ensure_driver(run)
            install.assert_not_called()
        self.assertFalse(any(command[0] == "dkms" for command in calls))

    def test_absent_core_exports_refuses_before_module_install(self):
        run, calls = self.runner()
        with tempfile.TemporaryDirectory() as directory:
            self.headers(directory, symbols="")
            with patch.object(driver, "_MODULE_ROOT", Path(directory)), \
                    patch.object(driver.platform, "release", return_value="6.12.1+rpt-rpi-2712"), \
                    patch.object(driver, "_install_sources") as install:
                with self.assertRaisesRegex(RuntimeError, "do not export"):
                    driver.ensure_driver(run)
            install.assert_not_called()
        self.assertFalse(any(command[0] == "dkms" for command in calls))

    def test_build_targets_running_kernel_and_installs_only_gpio_module(self):
        run, calls = self.runner()
        with tempfile.TemporaryDirectory() as directory:
            self.headers(directory)
            with patch.object(driver, "_MODULE_ROOT", Path(directory)), \
                    patch.object(driver.platform, "release", return_value="6.12.1+rpt-rpi-2712"), \
                    patch.object(driver, "_install_sources") as install:
                self.assertFalse(driver.ensure_driver(run))
            install.assert_called_once_with()
        builds = [command for command in calls if command[:2] == ["dkms", "build"]]
        self.assertEqual(len(builds), 1)
        self.assertEqual(builds[0][-2:], ["-k", "6.12.1+rpt-rpi-2712"])
        self.assertEqual(calls[-1], ["modprobe", "cec-gpio"])
        self.assertNotIn("CONFIG_CEC_PIN", driver._MAKEFILE.replace("$(CONFIG_CEC_PIN)", ""))
        self.assertNotIn("-DCONFIG_CEC_PIN", driver._MAKEFILE)

    def test_tampered_upstream_download_is_rejected(self):
        with patch.object(driver.urllib.request, "urlopen", return_value=BytesIO(b"changed upstream source")):
            with self.assertRaisesRegex(RuntimeError, "source hash mismatch"):
                driver._download("6.12", "cec-pin.c")

    def test_future_kernel_upgrade_fails_dkms_prebuild_instead_of_building_wrong_abi(self):
        with tempfile.TemporaryDirectory() as directory:
            prepare = Path(directory) / "prepare.py"
            prepare.write_text(driver._PREPARE)
            result = subprocess.run([sys.executable, str(prepare), "7.0.0-rpi"], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("supports kernel series", result.stderr)
            self.assertFalse((Path(directory) / "pin.c").exists())

    def test_dkms_hook_names_an_executable_in_the_build_directory(self):
        with tempfile.TemporaryDirectory() as directory, \
                patch.object(driver, "_SOURCE_ROOT", Path(directory)), \
                patch.object(driver, "_download", return_value=b"test source"):
            driver._install_sources()
            source = Path(directory) / f"{driver._NAME}-{driver._VERSION}"
            config = (source / "dkms.conf").read_text()
            self.assertIn('PRE_BUILD="prepare.py ${kernelver}"', config)
            # DKMS prefixes the first hook word with its build directory. It
            # does not run an arbitrary command such as "python3 prepare.py".
            hook = source / "prepare.py"
            self.assertTrue(os.access(hook, os.X_OK))
            result = subprocess.run([str(hook), "7.0.0-rpi"], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("supports kernel series", result.stderr)


if __name__ == "__main__":
    unittest.main()
