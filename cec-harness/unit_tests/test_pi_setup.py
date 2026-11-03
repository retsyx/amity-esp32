"""Pi provisioning boundaries and readiness failures without a real Pi."""

from contextlib import redirect_stderr, redirect_stdout
import errno
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import stat
import subprocess
import sys
import tarfile
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from amity_harness.wiring import Wiring


_SPEC = importlib.util.spec_from_file_location("harness_pi_setup", Path(__file__).resolve().parents[1] / "setup.py")
setup = importlib.util.module_from_spec(_SPEC)
sys.modules[_SPEC.name] = setup
_SPEC.loader.exec_module(setup)


class PiSetupTests(unittest.TestCase):
    def setUp(self):
        self.wiring = Wiring.from_markdown()
        self.account = SimpleNamespace(pw_name="bench", pw_dir="/home/bench", pw_uid=1001, pw_gid=1001)

    def test_platform_failure_precedes_wiring_account_and_provisioning(self):
        with patch.object(setup, "validate_platform", side_effect=ValueError("Unsupported Pi")), \
                patch.object(setup.Wiring, "from_markdown") as wiring, \
                patch.object(setup, "account_for") as account, \
                patch.object(setup, "install") as install, \
                patch.object(setup, "run") as run, redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(setup.main([]), 1)
        self.assertIn("Unsupported Pi", errors.getvalue())
        wiring.assert_not_called()
        account.assert_not_called()
        install.assert_not_called()
        run.assert_not_called()

    def test_invalid_wiring_precedes_account_and_provisioning(self):
        invalid = SimpleNamespace(cec={"wire": 28}, reset={"dut": 4})
        with patch.object(setup, "validate_platform"), \
                patch.object(setup.Wiring, "from_markdown", return_value=invalid), \
                patch.object(setup, "account_for") as account, \
                patch.object(setup, "install") as install, \
                patch.object(setup, "run") as run, redirect_stderr(io.StringIO()):
            self.assertEqual(setup.main([]), 1)
        account.assert_not_called()
        install.assert_not_called()
        run.assert_not_called()

    def test_malformed_boot_file_precedes_package_or_system_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            boot = Path(directory)
            (boot / "config.txt").write_text("[all]\n# BEGIN Amity harness\n")
            with patch.object(setup, "BOOT", boot), patch.object(setup, "run") as run, \
                    patch.object(setup, "write_file") as write:
                with self.assertRaisesRegex(ValueError, "Unterminated"):
                    setup.install(self.account, self.wiring)
            run.assert_not_called()
            write.assert_not_called()

    def test_check_dispatch_never_calls_provisioning(self):
        with patch.object(setup, "validate_platform"), \
                patch.object(setup.Wiring, "from_markdown", return_value=self.wiring), \
                patch.object(setup, "account_for", return_value=self.account), \
                patch.object(setup.os, "geteuid", return_value=self.account.pw_uid), \
                patch.object(setup, "check_ready") as check, \
                patch.object(setup, "install") as install, \
                patch.object(setup, "run") as run:
            self.assertEqual(setup.main(["--check"]), 0)
        check.assert_called_once_with(self.wiring)
        install.assert_not_called()
        run.assert_not_called()

    def test_root_check_switches_to_normal_account_without_installing(self):
        with patch.object(setup, "validate_platform"), \
                patch.object(setup.Wiring, "from_markdown", return_value=self.wiring), \
                patch.object(setup, "account_for", return_value=self.account), \
                patch.object(setup.os, "geteuid", return_value=0), \
                patch.object(setup, "as_user", return_value=SimpleNamespace(returncode=0)) as as_user, \
                patch.object(setup, "install") as install:
            self.assertEqual(setup.main(["--check"]), 0)
        as_user.assert_called_once_with(self.account, [sys.executable, "-B", str(setup.HARNESS / "setup.py"), "--check"])
        install.assert_not_called()

    def test_remote_check_does_not_deploy_archive_or_request_sudo(self):
        calls = []

        def run(command, **kwargs):
            calls.append(command)
            if len(calls) == 1:
                return SimpleNamespace(returncode=0, stdout='{"home":"/home/bench","boot":"original"}')
            return SimpleNamespace(returncode=0, stdout="")

        with patch.object(setup, "run", side_effect=run), \
                patch.object(setup, "source_archive") as archive, \
                patch.object(setup.subprocess, "run") as direct:
            self.assertEqual(setup.main(["--host", "bench@new-harness.local", "--check"]), 0)
        archive.assert_not_called()
        direct.assert_not_called()
        self.assertEqual(len(calls), 2)
        self.assertIn("--check", calls[-1][-1])
        self.assertTrue(all("StrictHostKeyChecking=yes" in command for command in calls))
        self.assertTrue(all("sudo" not in command[-1] and "tar " not in command[-1] for command in calls))

    def test_remote_install_resumes_after_header_and_overlay_reboots(self):
        installs = iter([setup.REBOOT_REQUIRED, setup.REBOOT_REQUIRED, 0])
        boot_ids = iter(["original", "headers-boot", "headers-boot", "overlay-boot"])
        commands = []

        def run(command, **kwargs):
            remote = command[-1]
            commands.append(remote)
            if len(commands) == 1:
                return SimpleNamespace(returncode=0, stdout='{"home":"/home/bench","boot":"original"}')
            if remote.startswith("sudo -n python3"):
                return SimpleNamespace(returncode=next(installs), stdout="")
            if remote == "cat /proc/sys/kernel/random/boot_id":
                return SimpleNamespace(returncode=0, stdout=next(boot_ids) + "\n")
            return SimpleNamespace(returncode=0, stdout="")

        with patch.object(setup, "run", side_effect=run), \
                patch.object(setup, "source_archive", return_value=b"source-archive") as archive, \
                patch.object(setup.subprocess, "run") as deploy, \
                patch.object(setup.time, "sleep"), redirect_stdout(io.StringIO()):
            self.assertEqual(setup.main(["--host", "bench@new-harness.local"]), 0)
        self.assertEqual(sum(command.startswith("sudo -n python3") for command in commands), 3)
        self.assertEqual(commands.count("sudo -n reboot"), 2)
        self.assertEqual(commands.count("cat /proc/sys/kernel/random/boot_id"), 4)
        archive.assert_called_once_with()
        deploy.assert_called_once()
        self.assertEqual(deploy.call_args.kwargs["input"], b"source-archive")
        self.assertIn("/home/bench/amity/esp32", deploy.call_args.args[0][-1])
        self.assertIn("StrictHostKeyChecking=accept-new", deploy.call_args.args[0])

    def test_ssh_bootstrap_accepts_only_new_keys_and_checks_require_known_keys(self):
        bootstrap = setup.ssh_command("bench@new-harness.local", 2222, bootstrap=True)
        check = setup.ssh_command("bench@new-harness.local", 2222)
        self.assertIn("StrictHostKeyChecking=accept-new", bootstrap)
        self.assertIn("StrictHostKeyChecking=yes", check)
        for command in (bootstrap, check):
            self.assertIn("BatchMode=yes", command)
            self.assertEqual(command[-3:], ["-p", "2222", "bench@new-harness.local"])

    def test_archive_contains_only_sources_never_private_or_cached_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "harness"
            expected = {"setup.py", "setup_matter.py", "run.py", "fw-reset", "README.md",
                        "TEST-HARNESS.md", "matter-requirements.txt", "amity_harness/__init__.py",
                        "functional_tests/example.py", "unit_tests/test_example.py"}
            excluded = {"credentials/enrollment.txt", "captures/trace.md", ".cache/dependency.py",
                        "amity_harness/__pycache__/cached.py", "private.txt", "private.json"}
            for name in expected | excluded:
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text("private" if name in excluded else "source")
            external = Path(directory) / "private.py"
            external.write_text("never deploy")
            (root / "amity_harness/linked.py").symlink_to(external)
            (root / "linked-directory").symlink_to(external.parent, target_is_directory=True)
            with patch.object(setup, "HARNESS", root):
                archive = setup.source_archive()
            with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as contents:
                members = contents.getmembers()
                self.assertTrue(all(item.isfile() for item in members))
                self.assertEqual({item.name for item in members}, {"cec-harness/" + name for name in expected})
                self.assertTrue(all(contents.extractfile(item).read() == b"source" for item in members))

    def test_as_user_refreshes_groups_and_sets_the_harness_environment(self):
        with patch.object(setup.os, "geteuid", return_value=0), patch.object(setup, "run") as run:
            setup.as_user(self.account, ["pinctrl", "get", "4"], capture_output=True)
        command = run.call_args.args[0]
        self.assertEqual(command[:5], ["runuser", "-u", "bench", "--", "env"])
        self.assertIn("HOME=/home/bench", command)
        self.assertIn("USER=bench", command)
        self.assertIn("LOGNAME=bench", command)
        self.assertIn("PYTHONDONTWRITEBYTECODE=1", command)
        self.assertIn("PYTHONPATH=" + str(setup.HARNESS), command)
        self.assertEqual(command[-3:], ["pinctrl", "get", "4"])
        self.assertEqual(run.call_args.kwargs, {"capture_output": True})

    def test_ready_rejects_stale_boot_settings_source_or_binary_before_hardware(self):
        for stale in ("boot", "source", "binary"):
            with self.subTest(stale=stale), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                boot, record = root / "boot", root / "overlay.json"
                (boot / "overlays").mkdir(parents=True)
                configuration = setup.boot_config("", self.wiring)
                (boot / "config.txt").write_text(configuration)
                binary = boot / "overlays/amity-harness.dtbo"
                binary.write_bytes(b"original compiled overlay")
                expected = {"source": hashlib.sha256(setup.overlay_source(self.wiring).encode()).hexdigest(),
                            "binary": hashlib.sha256(binary.read_bytes()).hexdigest()}
                if stale == "boot":
                    (boot / "config.txt").write_text(configuration.replace("dtoverlay=amity-harness", "dtoverlay=other"))
                elif stale == "source":
                    expected["source"] = "0" * 64
                else:
                    binary.write_bytes(b"changed compiled overlay")
                record.write_text(json.dumps(expected))
                with patch.object(setup, "BOOT", boot), patch.object(setup, "OVERLAY_RECORD", record), \
                        patch.object(setup.os, "geteuid", return_value=self.account.pw_uid), \
                        patch.object(setup, "check_cec") as cec, patch.object(setup, "run") as run:
                    with self.assertRaisesRegex(ValueError, "boot wiring|CEC overlay"):
                        setup.check_ready(self.wiring)
                cec.assert_not_called()
                run.assert_not_called()

    def test_ready_with_matching_record_advances_to_hardware(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            boot, record = root / "boot", root / "overlay.json"
            (boot / "overlays").mkdir(parents=True)
            (boot / "config.txt").write_text(setup.boot_config("", self.wiring))
            binary = boot / "overlays/amity-harness.dtbo"
            binary.write_bytes(b"compiled overlay")
            record.write_text(json.dumps({
                "source": hashlib.sha256(setup.overlay_source(self.wiring).encode()).hexdigest(),
                "binary": hashlib.sha256(binary.read_bytes()).hexdigest()}))
            with patch.object(setup, "BOOT", boot), patch.object(setup, "OVERLAY_RECORD", record), \
                    patch.object(setup.os, "geteuid", return_value=self.account.pw_uid), \
                    patch.object(setup, "check_cec", side_effect=RuntimeError("hardware reached")) as cec:
                with self.assertRaisesRegex(RuntimeError, "hardware reached"):
                    setup.check_ready(self.wiring)
            cec.assert_called_once_with(self.wiring)

    def test_pending_reboot_blocks_check_without_probes_or_marker_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            record, boot_id = Path(directory) / "pending", Path(directory) / "boot-id"
            record.write_text("original-boot\n")
            boot_id.write_text("original-boot\n")
            with patch.object(setup, "REBOOT_RECORD", record), patch.object(setup, "BOOT_ID", boot_id), \
                    patch.object(setup.os, "geteuid", return_value=self.account.pw_uid), \
                    patch.object(setup, "overlay_source") as source, \
                    patch.object(setup, "check_cec") as cec, patch.object(setup, "run") as run:
                self.assertTrue(setup.awaiting_reboot())
                with self.assertRaisesRegex(ValueError, "Reboot required"):
                    setup.check_ready(self.wiring)
                self.assertEqual(record.read_text(), "original-boot\n")
                boot_id.write_text("new-boot\n")
                self.assertFalse(setup.awaiting_reboot())
                self.assertEqual(record.read_text(), "original-boot\n")
            source.assert_not_called()
            cec.assert_not_called()
            run.assert_not_called()

    def test_install_rerun_preserves_pending_reboot_until_postboot_readiness_succeeds(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            boot, record, boot_id = root / "boot", root / "pending", root / "boot-id"
            boot.mkdir()
            (boot / "config.txt").write_text("[all]\n")
            boot_id.write_text("original-boot\n")
            real_write = setup.write_file

            def write(path, content, **kwargs):
                path = Path(path)
                if str(path).startswith("/etc/"):
                    path = root / "system" / path.relative_to("/")
                return real_write(path, content, **kwargs)

            def run(command, **kwargs):
                if command[0] == "dtc":
                    Path(command[command.index("-o") + 1]).write_bytes(b"compiled overlay")
                return SimpleNamespace(returncode=0, stdout="")

            with patch.object(setup, "BOOT", boot), \
                    patch.object(setup, "OVERLAY_RECORD", root / "overlay.json"), \
                    patch.object(setup, "REBOOT_RECORD", record), patch.object(setup, "BOOT_ID", boot_id), \
                    patch.object(setup, "run", side_effect=run), \
                    patch.object(setup, "write_file", side_effect=write), \
                    patch.object(setup, "retire_followers"), patch.object(setup, "configure_access"), \
                    patch("amity_harness.pi_cec_driver.ensure_driver", return_value=False), \
                    patch.object(setup, "as_user", return_value=SimpleNamespace(returncode=0)) as as_user, \
                    redirect_stdout(io.StringIO()):
                self.assertEqual(setup.install(self.account, self.wiring), setup.REBOOT_REQUIRED)
                self.assertEqual(record.read_text(), "original-boot\n")
                self.assertEqual(setup.install(self.account, self.wiring), setup.REBOOT_REQUIRED)
                self.assertEqual(record.read_text(), "original-boot\n")
                self.assertFalse(any(call.args[1][-1] == "--check" for call in as_user.call_args_list))

                boot_id.write_text("new-boot\n")
                self.assertFalse(setup.awaiting_reboot())

                def failed_readiness(account, command, **kwargs):
                    if command[-1] == "--check":
                        raise subprocess.CalledProcessError(1, command)
                    return SimpleNamespace(returncode=0)

                as_user.side_effect = failed_readiness
                with self.assertRaises(subprocess.CalledProcessError):
                    setup.install(self.account, self.wiring)
                self.assertTrue(record.exists())
                as_user.side_effect = None
                self.assertEqual(setup.install(self.account, self.wiring), 0)
                self.assertFalse(record.exists())
                self.assertEqual(as_user.call_args.args[1][-1], "--check")

    def test_atomic_write_retains_original_backup_and_repairs_unchanged_permissions(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "configuration"
            target.write_text("original")
            self.assertTrue(setup.write_file(target, "first", mode=0o440, backup=True))
            backup = target.with_name("configuration.before-amity-harness")
            self.assertEqual(backup.read_text(), "original")
            self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o440)
            target.chmod(0o644)
            inode = target.stat().st_ino
            self.assertFalse(setup.write_file(target, "first", mode=0o440, backup=True))
            self.assertEqual(target.stat().st_ino, inode)
            self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o440)
            self.assertTrue(setup.write_file(target, "second", mode=0o440, backup=True))
            self.assertEqual(target.read_text(), "second")
            self.assertEqual(backup.read_text(), "original")
            self.assertEqual({item.name for item in target.parent.iterdir()}, {target.name, backup.name})

    def _check_cec(self, devices, *, open_error=None):
        """Supply Linux adapter identities/capabilities while retaining real checks."""
        paths = ["/dev/cec" + str(index) for index in range(len(devices))]
        claims = []

        def ioctl(fd, direction, number, caps):
            if number == 9:
                self.assertEqual(direction, 1)
                self.assertEqual(caps.value, 0x32)
                claims.append(fd)
                return caps
            self.assertEqual((direction, number), (3, 0))
            driver, name, capabilities, addresses = devices[fd]
            caps.driver = driver.encode()
            caps.name = name.encode()
            caps.capabilities = capabilities
            caps.available_log_addrs = addresses
            return caps

        with patch.object(setup.glob, "glob", return_value=paths), \
                patch.object(setup.os, "open", side_effect=open_error or range(len(paths))) as opened, \
                patch.object(setup.os, "close") as closed, \
                patch.object(setup.fcntl, "flock") as locked, \
                patch("amity_harness.cec._ioctl", side_effect=ioctl):
            setup.check_cec(self.wiring)
        self.assertEqual(len(claims), sum(item[0] == "cec-gpio" for item in devices))
        return opened, closed, locked

    def test_cec_readiness_accepts_every_wire_without_numbering_assumptions(self):
        devices = [("cec-gpio", f"cec@{pin}", 0x0f, 4) for pin in reversed(self.wiring.cec.values())]
        devices.insert(0, ("vc4_hdmi", "HDMI", 0x0f, 1))
        opened, closed, locked = self._check_cec(devices)
        self.assertEqual(opened.call_count, len(devices))
        self.assertEqual(closed.call_count, len(devices))
        self.assertEqual(locked.call_count, len(self.wiring.cec))

    def test_cec_readiness_rejects_missing_duplicate_unexpected_and_incapable_adapters(self):
        valid = [("cec-gpio", f"cec@{pin}", 0x0f, 4) for pin in self.wiring.cec.values()]
        cases = [(valid[:-1], "missing"), (valid + valid[:1], "duplicate"),
                 (valid + [("cec-gpio", "cec@999", 0x0f, 4)], "Unexpected"),
                 ([("cec-gpio", valid[0][1], 0x07, 4)] + valid[1:], "capabilities"),
                 ([("cec-gpio", valid[0][1], 0x0f, 1)] + valid[1:], "capabilities")]
        for devices, message in cases:
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, message):
                self._check_cec(devices)

    def test_cec_readiness_rejects_inaccessible_device(self):
        with self.assertRaises(PermissionError):
            self._check_cec([("cec-gpio", "cec@23", 0x0f, 4)], open_error=PermissionError("denied"))

    def test_cec_readiness_releases_descriptor_if_claim_is_busy(self):
        with patch.object(setup.glob, "glob", return_value=["/dev/cec0"]), \
                patch.object(setup.os, "open", return_value=7), \
                patch.object(setup.os, "close") as closed, \
                patch.object(setup.fcntl, "flock", side_effect=BlockingIOError("busy")), \
                patch("amity_harness.cec._ioctl", return_value=SimpleNamespace(
                    driver=b"cec-gpio", name=f"cec@{next(iter(self.wiring.cec.values()))}".encode(),
                    capabilities=0x0f, available_log_addrs=4)):
            with self.assertRaises(BlockingIOError):
                setup.check_cec(self.wiring)
        closed.assert_called_once_with(7)

    def test_cec_readiness_releases_descriptor_when_kernel_exclusive_claim_is_denied(self):
        first_pin = next(iter(self.wiring.cec.values()))

        def ioctl(fd, direction, number, value):
            if number == 0:
                return SimpleNamespace(driver=b"cec-gpio", name=f"cec@{first_pin}".encode(),
                                       capabilities=0x0f, available_log_addrs=4)
            self.assertEqual((fd, direction, number, value.value), (7, 1, 9, 0x32))
            raise OSError(errno.EBUSY, "CEC follower already active")

        with patch.object(setup.glob, "glob", return_value=["/dev/cec0"]), \
                patch.object(setup.os, "open", return_value=7), \
                patch.object(setup.os, "close") as closed, \
                patch.object(setup.fcntl, "flock"), \
                patch("amity_harness.cec._ioctl", side_effect=ioctl):
            with self.assertRaises(OSError) as raised:
                setup.check_cec(self.wiring)
        self.assertEqual(raised.exception.errno, errno.EBUSY)
        closed.assert_called_once_with(7)


if __name__ == "__main__":
    unittest.main()
