"""Standalone runtime installation and verification without hardware or downloads."""

from contextlib import redirect_stderr, redirect_stdout
import importlib.util
import io
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch


_SPEC = importlib.util.spec_from_file_location("harness_runtime_setup", Path(__file__).resolve().parents[1] / "setup_runtime.py")
setup = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(setup)


class RuntimeSetupTests(unittest.TestCase):
    def test_invalid_manifest_precedes_installation(self):
        for content in ("pyserial>=3.5\n", "", "pyserial==3.5\npyserial==3.4\n"):
            with self.subTest(content=content), tempfile.TemporaryDirectory() as directory:
                requirements = Path(directory) / "requirements.txt"
                requirements.write_text(content)
                with patch.object(setup, "REQUIREMENTS", requirements), patch.object(setup.subprocess, "run") as run:
                    with self.assertRaises(ValueError):
                        setup.install(Path(directory) / "runtime")
                run.assert_not_called()

    def test_install_keeps_system_ble_packages_and_uses_the_runtime_interpreter(self):
        destination = Path("/home/bench/runtime with spaces")
        with patch.object(setup.subprocess, "run") as run, patch.object(setup, "verify_installed") as verify:
            setup.install(destination)
        self.assertEqual(run.call_args_list[0].args[0],
                         [sys.executable, "-m", "venv", "--system-site-packages", str(destination)])
        self.assertEqual(run.call_args_list[1].args[0],
                         [str(destination / "bin/python"), "-m", "pip", "install",
                          "--disable-pip-version-check", "--requirement", str(setup.REQUIREMENTS)])
        verify.assert_called_once_with(destination)

    def test_check_mode_never_installs(self):
        with patch.object(setup, "validate_platform"), patch.object(setup, "verify_installed") as verify, \
                patch.object(setup, "install") as install:
            self.assertEqual(setup.main(["--verify-installed"]), 0)
        verify.assert_called_once_with(setup.DESTINATION)
        install.assert_not_called()

    def test_missing_runtime_and_missing_system_packages_fail_without_launch(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory)
            with patch.object(setup.subprocess, "run") as run:
                with self.assertRaisesRegex(ValueError, "missing"):
                    setup.verify_installed(destination)
                (destination / "bin").mkdir()
                (destination / "bin/python").touch()
                (destination / "pyvenv.cfg").write_text("include-system-site-packages = false\n")
                with self.assertRaisesRegex(ValueError, "system packages"):
                    setup.verify_installed(destination)
                run.assert_not_called()

    def test_verification_uses_runtime_python_and_has_a_deadline(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory)
            (destination / "bin").mkdir()
            (destination / "bin/python").touch()
            (destination / "pyvenv.cfg").write_text("include-system-site-packages = true\n")
            with patch.object(setup.subprocess, "run") as run:
                setup.verify_installed(destination)
            run.assert_called_once_with(
                [str(destination / "bin/python"), "-B", str(Path(setup.__file__).resolve()), "--verify-environment"],
                check=True, timeout=60)

    def test_wrong_pin_fails_before_importing_runtime_or_launching_browser(self):
        with patch.object(setup.metadata, "version", return_value="wrong"), \
                patch.object(setup.shutil, "which") as which:
            with self.assertRaisesRegex(ValueError, "differs from"):
                setup.verify_environment()
        which.assert_not_called()

    def test_environment_check_imports_without_opening_usb_and_closes_browser(self):
        serial = SimpleNamespace(Serial=Mock(side_effect=AssertionError("USB must stay closed")))
        ports = SimpleNamespace(comports=Mock(side_effect=AssertionError("No USB enumeration")))
        reset = SimpleNamespace(hard_reset=Mock(side_effect=AssertionError("No reset")))
        browser, driver = Mock(), Mock()
        driver.chromium.launch.return_value = browser
        context = Mock()
        context.__enter__ = Mock(return_value=driver)
        context.__exit__ = Mock(return_value=False)
        playwright = SimpleNamespace(sync_playwright=Mock(return_value=context))
        modules = {"serial": serial, "serial.tools": SimpleNamespace(list_ports=ports),
                   "serial.tools.list_ports": ports, "esp_pylib": SimpleNamespace(serial_reset=reset),
                   "esp_pylib.serial_reset": reset, "dbus_next": SimpleNamespace(),
                   "playwright": SimpleNamespace(sync_api=playwright), "playwright.sync_api": playwright}
        versions = dict(setup.pinned_requirements(setup.REQUIREMENTS))
        with patch.dict(sys.modules, modules), patch.object(setup.metadata, "version", side_effect=versions.__getitem__), \
                patch.object(setup.shutil, "which", return_value="/usr/bin/chromium"), redirect_stdout(io.StringIO()):
            setup.verify_environment()
        serial.Serial.assert_not_called()
        ports.comports.assert_not_called()
        reset.hard_reset.assert_not_called()
        driver.chromium.launch.assert_called_once_with(executable_path="/usr/bin/chromium", headless=True, timeout=30000)
        browser.close.assert_called_once_with()
        context.__exit__.assert_called_once()

    def test_wrapper_reports_missing_runtime_and_preserves_all_arguments(self):
        with tempfile.TemporaryDirectory(prefix="harness runtime ") as directory:
            root = Path(directory)
            harness = root / "cec-harness"
            harness.mkdir()
            wrapper = harness / "run"
            shutil.copyfile(setup.HARNESS / "run", wrapper)
            wrapper.chmod(0o755)
            missing = subprocess.run([str(wrapper)], capture_output=True, text=True)
            self.assertEqual(missing.returncode, 1)
            self.assertIn('rerun: sudo python3 "' + str(harness / "setup.py") + '"', missing.stderr)
            python = root / ".harness-python/bin/python"
            python.parent.mkdir(parents=True)
            python.write_text("#!/bin/sh\nprintf '%s\\n' \"$@\"\n")
            python.chmod(0o755)
            arguments = ["--credentials", "/private/path with spaces.json", "literal;$(touch never-created)"]
            result = subprocess.run([str(wrapper), *arguments], cwd=root, capture_output=True, text=True, check=True)
            self.assertEqual(result.stdout.splitlines(), ["-B", str(harness / "run.py"), *arguments])
            self.assertFalse((root / "never-created").exists())


if __name__ == "__main__":
    unittest.main()
