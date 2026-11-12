"""USB flashing command construction and failure reporting without touching hardware."""

import io
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import Mock, patch

from amity_harness import flashing
from amity_harness.wiring import Wiring


HARNESS = Path(flashing.__file__).resolve().parents[1]


class FakeWorker:
    def __init__(self, command, stdout, code=0, output=b"", hang=False):
        self.command, self.code, self.hang = command, code, hang
        self.killed = self.waited = False
        stdout.write(output)
        stdout.flush()
        otadata = Path(command[command.index(hex(flashing.OTADATA_OFFSET)) + 1])
        self.otadata = otadata.read_bytes()

    def wait(self, timeout=None):
        if self.hang and not self.killed:
            raise subprocess.TimeoutExpired(self.command, timeout)
        self.waited = True
        return -9 if self.killed else self.code

    def kill(self):
        self.killed = True


class FlashingTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.image = self.root / "amity-partition.bin"
        self.image.write_bytes(b"\xe9" + b"\xff" * (flashing.SLOT_BYTES - 1))
        self.wiring = Wiring.from_markdown()
        self.targets = [{"dut": identity.name, "usb_serial": identity.mac.upper(), "port": f"/dev/ttyACM{index}"}
                        for index, identity in enumerate(self.wiring.identities.values())]

    def tearDown(self):
        self.directory.cleanup()

    def flash(self, popen, **kwargs):
        out, err = io.StringIO(), io.StringIO()
        with patch.object(flashing, "verify_usb", return_value=self.targets) as verify:
            failed = flashing.flash_all(self.image, wiring=self.wiring, python="/runtime/python",
                                        popen=popen, out=out, err=err, **kwargs)
        verify.assert_called_once_with(self.wiring)
        return failed, out.getvalue(), err.getvalue()

    def test_invalid_images_are_rejected_before_usb_or_flashing(self):
        bundle = self.root / "amity-update.bin"
        bundle.write_bytes(b"AMITY-UPDATE-V1\n" + b"\0" * 64)
        short = self.root / "amity.bin"
        short.write_bytes(b"\xe9" + b"\0" * 1000)
        wrong_magic = self.root / "blank.bin"
        wrong_magic.write_bytes(b"\xff" * flashing.SLOT_BYTES)
        popen = Mock()
        for path in (bundle, short, wrong_magic, self.root / "missing.bin"):
            with self.subTest(path=path.name), patch.object(flashing, "verify_usb") as verify:
                with self.assertRaisesRegex(ValueError, path.name):
                    flashing.flash_all(path, wiring=self.wiring, popen=popen)
                verify.assert_not_called()
        popen.assert_not_called()

    def test_every_documented_dut_is_flashed_in_parallel_with_only_otadata_and_ota_0(self):
        workers = []

        def popen(command, **kwargs):
            self.assertFalse(any(worker.waited for worker in workers), "All boards start before any wait")
            self.assertEqual(kwargs["stdin"], subprocess.DEVNULL)
            self.assertEqual(kwargs["stderr"], subprocess.STDOUT)
            workers.append(FakeWorker(command, kwargs["stdout"]))
            return workers[-1]

        failed, out, err = self.flash(popen)
        self.assertEqual(failed, [])
        self.assertEqual(err, "")
        self.assertEqual(len(workers), len(self.targets))
        for worker, target in zip(workers, self.targets):
            otadata = worker.command[-3]
            self.assertEqual(worker.command, [
                "/runtime/python", "-m", "esptool", "--chip", "esp32s3", "--port", target["port"],
                "--baud", "460800", "--before", "default-reset", "--after", "hard-reset", "write-flash",
                "0x21000", otadata, "0x40000", str(self.image.resolve())])
            self.assertEqual(worker.otadata, b"\xff" * 0x2000)
            self.assertIn(f"{target['dut']}: flashed amity-partition.bin on {target['port']}", out)
        self.assertFalse(Path(workers[0].command[-3]).exists(), "Temporary OTA data is removed")

    def test_failure_is_reported_per_dut_without_retry(self):
        workers = []

        def popen(command, **kwargs):
            port = command[command.index("--port") + 1]
            failing = port == self.targets[1]["port"]
            workers.append(FakeWorker(command, kwargs["stdout"], code=2 if failing else 0,
                                      output=b"Connecting...\nA fatal error occurred: No serial data\n"
                                      if failing else b"Hash of data verified.\n"))
            return workers[-1]

        failed, out, err = self.flash(popen)
        self.assertEqual(failed, [self.targets[1]["dut"]])
        self.assertEqual(len(workers), len(self.targets))
        self.assertIn(f"{self.targets[1]['dut']}: FAILED (2)", err)
        self.assertIn("A fatal error occurred: No serial data", err)
        self.assertNotIn(f"{self.targets[1]['dut']}: flashed", out)
        self.assertIn(f"{self.targets[0]['dut']}: flashed", out)

    def test_a_hung_flash_is_killed_and_reported(self):
        workers = []

        def popen(command, **kwargs):
            workers.append(FakeWorker(command, kwargs["stdout"], hang=not workers))
            return workers[-1]

        failed, _, err = self.flash(popen, timeout=1)
        self.assertEqual(failed, [self.targets[0]["dut"]])
        self.assertTrue(workers[0].killed)
        self.assertIn("FAILED (timeout)", err)

    def test_launch_failure_stops_boards_already_started(self):
        workers = []

        def popen(command, **kwargs):
            if workers:
                raise OSError("cannot start")
            workers.append(FakeWorker(command, kwargs["stdout"]))
            return workers[-1]

        with self.assertRaises(OSError):
            self.flash(popen)
        self.assertTrue(workers[0].killed)
        self.assertTrue(workers[0].waited)

    def test_main_reports_invalid_image_and_failed_boards(self):
        err = io.StringIO()
        with patch("sys.stderr", err), self.assertRaises(SystemExit) as exit_:
            flashing.main([str(self.root / "missing.bin")])
        self.assertEqual(exit_.exception.code, 1)
        self.assertIn("missing.bin", err.getvalue())
        with patch.object(flashing, "flash_all", return_value=["amity-dut-2"]), \
                patch("sys.stderr", io.StringIO()) as err:
            self.assertEqual(flashing.main([str(self.image)]), 1)
        self.assertIn("failed on amity-dut-2", err.getvalue())
        with patch.object(flashing, "flash_all", return_value=[]) as flash_all:
            self.assertEqual(flashing.main([str(self.image)]), 0)
        flash_all.assert_called_once_with(self.image)

    def test_wrapper_reports_missing_runtime_and_preserves_all_arguments(self):
        with tempfile.TemporaryDirectory(prefix="harness runtime ") as directory:
            root = Path(directory)
            harness = root / "cec-harness"
            harness.mkdir()
            wrapper = harness / "flash-duts"
            shutil.copyfile(HARNESS / "flash-duts", wrapper)
            wrapper.chmod(0o755)
            missing = subprocess.run([str(wrapper)], capture_output=True, text=True)
            self.assertEqual(missing.returncode, 1)
            self.assertIn('rerun: sudo python3 "' + str(harness / "setup.py") + '"', missing.stderr)
            python = root / ".harness-python/bin/python"
            python.parent.mkdir(parents=True)
            python.write_text("#!/bin/sh\nprintf '%s\\n' \"$PYTHONPATH\" \"$@\"\n")
            python.chmod(0o755)
            arguments = ["/private/path with spaces/amity-partition.bin", "literal;$(touch never-created)"]
            result = subprocess.run([str(wrapper), *arguments], cwd=root, capture_output=True, text=True, check=True)
            self.assertEqual(result.stdout.splitlines(),
                             [str(harness), "-B", "-m", "amity_harness.flashing", *arguments])
            self.assertFalse((root / "never-created").exists())


if __name__ == "__main__":
    unittest.main()
