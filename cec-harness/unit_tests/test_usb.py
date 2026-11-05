"""USB reboot targeting and failure handling without touching hardware."""

from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from amity_harness import DUT, RunConfig
from amity_harness.runner import main
from amity_harness.usb import USBReset, USBWorkerError, main as worker_main, reset_usb, verify_usb
from amity_harness import usb
from amity_harness.wiring import Wiring


class USBResetTests(unittest.TestCase):
    def test_reset_runs_one_bounded_worker_with_only_documented_serial(self):
        dut = DUT("amity-dut-2")
        reply = json.dumps({"usb_serial": dut.identity.mac, "port": "/dev/cu.native", "reset": True})
        runner = Mock(return_value=subprocess.CompletedProcess([], 0, reply, ""))
        reset = USBReset(python=Path("/tools/SDK Python/python"), timeout=12, runner=runner)
        self.assertEqual(reset(dut), "/dev/cu.native")
        runner.assert_called_once_with(
            ["/tools/SDK Python/python", str(Path(usb.__file__).resolve()), "--usb-serial", dut.identity.mac],
            check=False, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, timeout=12, text=True)

    def test_invalid_serial_cannot_select_an_arbitrary_adapter_or_command(self):
        runner = Mock()
        reset = USBReset(runner=runner)
        for serial in ("", None, "44:B1:76:AE:C5:E0; reset halt", "44:B1:76:AE:C5"):
            with self.subTest(serial=serial), self.assertRaises(ValueError):
                reset(SimpleNamespace(name="amity-dut-2", identity=SimpleNamespace(mac=serial)))
        runner.assert_not_called()

    def test_worker_failure_and_timeout_preserve_private_diagnostics_without_retry(self):
        raw = json.dumps({"error": "USB_RESET_FAILED", "error_type": "SerialException"})
        runner = Mock(return_value=subprocess.CompletedProcess([], 1, raw, "PRIVATE_DIAGNOSTICS"))
        with self.assertRaisesRegex(RuntimeError, "USB_RESET_FAILED: SerialException") as raised:
            USBReset(runner=runner)(DUT("amity-dut-2"))
        self.assertEqual(raised.exception.stdout, raw)
        self.assertEqual(raised.exception.stderr, "PRIVATE_DIAGNOSTICS")
        self.assertNotIn("PRIVATE_DIAGNOSTICS", str(raised.exception))
        runner.assert_called_once()
        runner = Mock(side_effect=subprocess.TimeoutExpired(["python"], 12, output="PRIVATE_PARTIAL", stderr="PRIVATE_ERROR"))
        with self.assertRaisesRegex(RuntimeError, "timed out") as raised:
            USBReset(runner=runner)(DUT("amity-dut-2"))
        self.assertEqual(raised.exception.stdout, "PRIVATE_PARTIAL")
        self.assertEqual(raised.exception.stderr, "PRIVATE_ERROR")
        runner.assert_called_once()
        with self.assertRaisesRegex(RuntimeError, "could not start: FileNotFoundError"):
            USBReset(runner=Mock(side_effect=FileNotFoundError("PRIVATE_PATH")))(DUT("amity-dut-2"))

    def test_parent_requires_exact_worker_confirmation_and_sanitizes_failures(self):
        dut = DUT("amity-dut-2")
        valid = {"reset": True, "usb_serial": dut.identity.mac, "port": "/dev/cu.native"}
        for reply in ("not JSON", "[]", json.dumps({**valid, "reset": False}),
                      json.dumps({**valid, "usb_serial": "68:EE:8F:50:2B:44"})):
            with self.subTest(reply=reply), self.assertRaisesRegex(RuntimeError, "invalid confirmation"):
                USBReset(runner=Mock(return_value=subprocess.CompletedProcess([], 0, reply, "")))(dut)
        raw = json.dumps({"error": "PRIVATE_ERROR", "error_type": "PRIVATE DATA"})
        with self.assertRaisesRegex(RuntimeError, "USB_WORKER_FAILED$") as raised:
            USBReset(runner=Mock(return_value=subprocess.CompletedProcess([], 1, raw, "")))(dut)
        self.assertNotIn("PRIVATE", str(raised.exception))

    def test_invalid_deadlines_and_run_config_callbacks_are_rejected(self):
        for timeout in (True, 0, -1, float("nan"), float("inf"), "30"):
            with self.subTest(timeout=timeout), self.assertRaises(ValueError):
                USBReset(timeout=timeout)
        with self.assertRaises(ValueError):
            RunConfig(usb_reset=False)

    def test_cli_uses_current_pi_runtime_for_usb_reset(self):
        observed = []

        def scenario():
            pass

        def capture(test, config):
            observed.append(config.usb_reset)
            return {"test": test.__name__, "passed": True, "testbeds": []}

        with patch("amity_harness.runner.run_test", side_effect=capture), redirect_stdout(io.StringIO()):
            self.assertEqual(main(scenario, ["--credentials", "/private/main.json", "--no-status-led"]), 0)
        self.assertIsInstance(observed[0], USBReset)
        self.assertEqual(observed[0].python, sys.executable)


class USBInventoryTests(unittest.TestCase):
    def setUp(self):
        self.wiring = Wiring.from_markdown()
        self.ports = [SimpleNamespace(vid=0x303A, pid=0x1001,
                                     serial_number=identity.mac.lower(), device=f"/dev/ttyACM{index}")
                      for index, identity in enumerate(self.wiring.identities.values())]

    def test_verification_matches_every_documented_identity_without_opening_ports(self):
        unrelated = SimpleNamespace(vid=0x303A, pid=0x1001,
                                    serial_number="00:00:00:00:00:01", device="/dev/ttyACM9")
        access = Mock(return_value=True)
        with patch("builtins.open", side_effect=AssertionError("Serial inventory must not open files or ports")), \
                patch.object(usb, "reset_usb", side_effect=AssertionError("Serial inventory must not reset DUTs")):
            inventory = verify_usb(self.wiring, enumerate_ports=lambda: [unrelated, *reversed(self.ports)],
                                   check_access=access)
        self.assertEqual(inventory, [{"dut": identity.name, "usb_serial": identity.mac.upper(), "port": port.device}
                                    for identity, port in zip(self.wiring.identities.values(), self.ports)])
        self.assertEqual(access.call_count, len(self.wiring.identities))
        self.assertEqual(access.call_args.args[1], os.R_OK | os.W_OK)

    def test_inventory_rejects_missing_ambiguous_non_native_and_inaccessible_duts(self):
        first = next(iter(self.wiring.identities.values()))
        wrong_adapter = SimpleNamespace(**{**vars(self.ports[0]), "vid": 0x10C4, "pid": 0xEA60})
        for ports, accessible, code in (
                (self.ports[1:], True, "USB_DEVICE_NOT_FOUND"),
                ([*self.ports, self.ports[0]], True, "USB_DEVICE_AMBIGUOUS"),
                ([wrong_adapter, *self.ports[1:]], True, "USB_DEVICE_NOT_FOUND"),
                (self.ports, False, "USB_DEVICE_NOT_ACCESSIBLE")):
            with self.subTest(code=code), self.assertRaisesRegex(RuntimeError, f"{first.name}: {code}"):
                verify_usb(self.wiring, enumerate_ports=lambda: ports, check_access=lambda *args: accessible)

    def test_check_cli_uses_selected_wiring_and_never_resets(self):
        output = io.StringIO()
        with patch.object(Wiring, "from_markdown", return_value=self.wiring) as load, \
                patch.object(usb, "verify_usb", return_value=[{"dut": "selected"}]) as verify, \
                patch.object(usb, "reset_usb") as reset, redirect_stdout(output):
            self.assertEqual(worker_main(["--check", "--wiring", "/bench/wiring.md"]), 0)
        load.assert_called_once_with(Path("/bench/wiring.md"))
        verify.assert_called_once_with(self.wiring)
        reset.assert_not_called()
        self.assertEqual(json.loads(output.getvalue()), {"devices": [{"dut": "selected"}], "verified": True})


class USBWorkerTests(unittest.TestCase):
    def port(self, **changes):
        return SimpleNamespace(vid=0x303A, pid=0x1001, serial_number="44:B1:76:AE:C5:E0",
                               device="/dev/cu.native", **changes)

    def test_native_identity_selection_and_control_only_reset(self):
        selected = self.port()
        unrelated = SimpleNamespace(vid=0x303A, pid=0x1001, serial_number="68:EE:8F:50:2B:44", device="/dev/cu.other")
        events = []

        class Port:
            def __setattr__(self, name, value):
                events.append((name, value))
                object.__setattr__(self, name, value)

            def open(self):
                events.append(("open", self.port))

            def close(self):
                events.append(("close", self.port))

            def read(self, *args):
                raise AssertionError("A reboot must not read serial bytes")

            def write(self, *args):
                raise AssertionError("A reboot must not write serial bytes")

        port = Port()
        factory = Mock(return_value=port)

        def reset(target, **options):
            self.assertIs(target, port)
            self.assertIs(target.dtr, False)
            self.assertIs(target.rts, False)
            self.assertEqual(options, {"hold_delay": 0.2, "post_release_delay": 0.2})
            events.append(("hard_reset", target.port))

        primitive = Mock(side_effect=reset)
        result = reset_usb(selected.serial_number, serial_factory=factory,
                           enumerate_ports=lambda: [unrelated, selected], hard_reset=primitive)
        self.assertEqual(result, {"usb_serial": selected.serial_number, "port": selected.device, "reset": True})
        factory.assert_called_once_with(port=None, baudrate=115200, timeout=1, write_timeout=1, exclusive=True)
        primitive.assert_called_once()
        self.assertEqual(events, [("dtr", False), ("rts", False), ("port", selected.device),
                                  ("open", selected.device), ("hard_reset", selected.device), ("close", selected.device)])

    def test_missing_ambiguous_or_non_native_identity_rejects_before_open(self):
        valid = self.port()
        factory, primitive = Mock(), Mock()
        for ports, code in (([], "USB_DEVICE_NOT_FOUND"), ([valid, valid], "USB_DEVICE_AMBIGUOUS"),
                            ([SimpleNamespace(vid=0x10C4, pid=0xEA60, serial_number=valid.serial_number)], "USB_DEVICE_NOT_FOUND"),
                            ([SimpleNamespace(vid=0x303A, pid=0x1001, serial_number=None)], "USB_DEVICE_NOT_FOUND")):
            with self.subTest(code=code), self.assertRaisesRegex(USBWorkerError, code):
                reset_usb(valid.serial_number, serial_factory=factory, enumerate_ports=lambda: ports, hard_reset=primitive)
        factory.assert_not_called()
        primitive.assert_not_called()

    def test_worker_closes_port_on_open_or_control_failure_without_retry(self):
        for operation in ("open", "reset"):
            with self.subTest(operation=operation):
                port, primitive = Mock(), Mock()
                if operation == "open":
                    port.open.side_effect = OSError("PRIVATE_OPEN_ERROR")
                else:
                    primitive.side_effect = OSError("PRIVATE_RESET_ERROR")
                with self.assertRaises(OSError):
                    reset_usb(self.port().serial_number, serial_factory=Mock(return_value=port),
                              enumerate_ports=lambda: [self.port()], hard_reset=primitive)
                port.open.assert_called_once()
                port.close.assert_called_once()
                port.read.assert_not_called()
                port.write.assert_not_called()
                self.assertEqual(primitive.call_count, int(operation == "reset"))

    def test_worker_protocol_emits_only_safe_result_or_error_metadata(self):
        success = {"usb_serial": self.port().serial_number, "port": self.port().device, "reset": True}
        for result, status in ((success, 0), (ModuleNotFoundError("PRIVATE_IMPORT_PATH"), 1),
                               (OSError("PRIVATE_DEVICE_MESSAGE"), 1), (USBWorkerError("USB_DEVICE_NOT_FOUND"), 1)):
            with self.subTest(result=type(result).__name__):
                output = io.StringIO()
                effect = {"side_effect": result} if isinstance(result, Exception) else {"return_value": result}
                with patch.object(usb, "reset_usb", **effect), redirect_stdout(output):
                    self.assertEqual(worker_main(["--usb-serial", self.port().serial_number]), status)
                body = json.loads(output.getvalue())
                self.assertNotIn("PRIVATE", output.getvalue())
                if status:
                    self.assertEqual(set(body), {"error", "error_type"})
                else:
                    self.assertEqual(body, success)


if __name__ == "__main__":
    unittest.main()
