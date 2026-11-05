"""Bootstrap behavior without Bluetooth, DUT networking or native SDK."""

import asyncio
import io
import json
from pathlib import Path
import ssl
import stat
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import ANY, AsyncMock, Mock, patch

from amity_harness.dut import AmityHTTPError
from amity_harness.enrollment import (EnrollmentError, MatterEnrollment, _credentials,
                                      _commission_factory, _read_credentials, _request, _verify_factory,
                                      _verify_factory_and_release,
                                      _worker, ensure_enrolled)


CREDENTIALS = {"wifi_ssid": "private ssid $(literal)", "wifi_password": "private wifi password",
               "matter_pin_code": 20202021, "matter_discriminator": 3840}
DEVICE_ID = "a" * 32


class DUT:
    def __init__(self, credentials, *responses):
        self.name, self.identity = "amity-dut", SimpleNamespace(device_id=DEVICE_ID)
        self.url, self._opener = "https://amity-dut.local", self
        self._runtime = SimpleNamespace(credentials_for=lambda name: credentials)
        self._token = None
        self.responses = list(responses)
        self.requests = []
        self.presses = []
        self.closed = 0

    def open(self, request, timeout):
        if self.responses and isinstance(self.responses[0], (OSError, AmityHTTPError)):
            raise self.responses.pop(0)
        return io.BytesIO(json.dumps({"device_id": DEVICE_ID, "ready": True}).encode())

    def request(self, path, data=None, **options):
        self.requests.append((path, data, options))
        result = self.responses.pop(0)
        if isinstance(result, BaseException):
            raise result
        return result

    def press_fw_reset(self, seconds):
        self.presses.append(seconds)

    def close(self):
        self.closed += 1


class EnrollmentTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.credentials = self.root / "credentials.json"
        self.credentials.write_text(json.dumps({"admin_password": "factory admin"}))
        self.credentials.chmod(0o600)
        self.system = {"octopus_id": DEVICE_ID, "boot_id": "boot-1", "mode": "coordinator",
                       "ready": True, "configuration_pending": False,
                       "membership": {"coordinator": True, "authority_ready": True, "member": False}}

    def dut(self, *responses):
        return DUT(self.credentials, *responses)

    def test_reachable_main_fails_before_login_bluetooth_or_button(self):
        dut = self.dut({"token": "session"}, self.system)
        commissioner = Mock()
        with self.assertRaisesRegex(EnrollmentError, "must be factory reset"):
            ensure_enrolled(dut, commissioner)
        self.assertFalse(dut.requests)
        self.assertIsNone(dut._token)
        self.assertFalse(dut.presses)
        commissioner.commission.assert_not_called()

    def test_expired_initial_window_is_reopened_then_commissioned_once_and_http_verified(self):
        dut = self.dut(ConnectionRefusedError(), ConnectionRefusedError(), {"token": "fresh"}, self.system)
        commissioner = Mock()
        with patch("amity_harness.enrollment.time.sleep"):
            result = ensure_enrolled(dut, commissioner)
        self.assertEqual(dut.presses, [0.5])
        commissioner.commission.assert_called_once_with(dut, timeout=ANY)
        self.assertTrue(result["enrolled"])
        self.assertEqual(dut._token, "fresh")
        self.assertEqual([r[0] for r in dut.requests], ["/api/login"] * 2 + ["/api/system"])

    def test_existing_authenticated_session_is_never_reused(self):
        dut = self.dut(self.system)
        dut._token = "existing"
        with self.assertRaisesRegex(EnrollmentError, "must be factory reset"):
            ensure_enrolled(dut, Mock())
        self.assertFalse(dut.requests)

    def test_reachable_authentication_tls_and_identity_errors_never_start_enrollment(self):
        for responses, expected in (((AmityHTTPError(401, "wrong password"),), AmityHTTPError),
                                    ((ssl.SSLCertVerificationError("changed certificate"),), ssl.SSLError)):
            with self.subTest(expected=expected):
                dut, commissioner = self.dut(*responses), Mock()
                with self.assertRaises(expected):
                    ensure_enrolled(dut, commissioner)
                commissioner.commission.assert_not_called()
                self.assertFalse(dut.presses)
                self.assertIsNone(dut._token)

    def test_secondary_and_missing_private_http_credentials_fail_before_any_io(self):
        dut, commissioner = self.dut(), Mock()
        dut.name = "amity-dut-2"
        with self.assertRaises(ValueError):
            ensure_enrolled(dut, commissioner)
        dut.name = "amity-dut"
        self.credentials.chmod(0o644)
        with self.assertRaises(ValueError):
            ensure_enrolled(dut, commissioner)
        self.assertFalse(dut.requests or dut.presses)

    def test_commission_failure_propagates_without_automatic_factory_reset_or_retry(self):
        dut = self.dut(ConnectionRefusedError())
        commissioner = Mock()
        commissioner.commission.side_effect = EnrollmentError("failed")
        with self.assertRaises(EnrollmentError):
            ensure_enrolled(dut, commissioner)
        self.assertEqual(dut.presses, [0.5])
        commissioner.commission.assert_called_once()

    def test_readiness_timeout_does_not_recommission(self):
        dut, commissioner = self.dut(ConnectionRefusedError(), ConnectionRefusedError()), Mock()
        with patch("amity_harness.enrollment.time.monotonic", side_effect=[0, 0, 0, 0, 2, 2]), \
                patch("amity_harness.enrollment.time.sleep"), self.assertRaises(TimeoutError):
            ensure_enrolled(dut, commissioner, timeout=1)
        commissioner.commission.assert_called_once()
        self.assertEqual(dut.presses, [0.5])

    def enrollment_credentials(self):
        path = self.root / "enrollment.json"
        path.write_text(json.dumps(CREDENTIALS))
        path.chmod(0o600)
        return path

    def test_private_credentials_validate_without_echoing_secret_contents(self):
        path = self.enrollment_credentials()
        self.assertEqual(_read_credentials(path), CREDENTIALS)
        path.chmod(0o644)
        with self.assertRaises(ValueError):
            _read_credentials(path)
        for change in ({"matter_pin_code": True}, {"matter_pin_code": 11111111},
                       {"matter_discriminator": 4096}, {"wifi_ssid": ""},
                       {"wifi_password": "secret\x00"}, {"unexpected": "secret"}):
            with self.subTest(change=change), self.assertRaises(ValueError) as failure:
                _credentials({**CREDENTIALS, **change})
            self.assertNotIn("secret", str(failure.exception))

    def run_response(self, calls, mutate=None, failure=None):
        def runner(command, **options):
            calls.append((command, options))
            if failure:
                raise failure
            request = json.loads(options["input"])
            result = ({"commissioned": True, "node_id": 1} if request["operation"] == "enroll"
                      else {"factory_reset": True})
            response = {"protocol_version": 1, "request_id": request["request_id"],
                        "device_id": request["device_id"], "operation": request["operation"], **result}
            if mutate:
                mutate(response)
            return SimpleNamespace(returncode=0, stdout=json.dumps(response))
        return runner

    def test_local_worker_sends_secrets_only_on_stdin_with_bounded_timeout(self):
        calls = []
        client = MatterEnrollment(credentials=self.enrollment_credentials(), timeout=30,
                                  runner=self.run_response(calls))
        self.assertTrue(client.commission(self.dut())["commissioned"])
        self.assertEqual(len(calls), 1)
        command, options = calls[0]
        self.assertEqual(command[:4], [sys.executable, "-u", "-m", "amity_harness.enrollment"])
        self.assertEqual(json.loads(options["input"])["credentials"], CREDENTIALS)
        self.assertEqual(command[command.index("--timeout") + 1], "30")
        self.assertEqual(options["timeout"], 40)
        self.assertNotIn(CREDENTIALS["wifi_password"], str(command))
        self.assertNotIn(str(CREDENTIALS["matter_pin_code"]), str(command))

    def test_default_credentials_are_loaded_only_by_pi_worker(self):
        calls = []
        MatterEnrollment(runner=self.run_response(calls)).commission(self.dut())
        self.assertIsNone(json.loads(calls[0][1]["input"])["credentials"])

    def test_verify_factory_requests_only_verification_and_requires_matching_acknowledgment(self):
        calls = []
        result = MatterEnrollment(runner=self.run_response(calls)).verify_factory(self.dut())
        self.assertEqual(result, {"factory_reset": True, "device_id": DEVICE_ID})
        self.assertEqual(json.loads(calls[0][1]["input"])["operation"], "verify_factory")
        for change in (lambda r: r.update(factory_reset=False), lambda r: r.update(operation="enroll")):
            with self.subTest(change=change), self.assertRaises(EnrollmentError):
                MatterEnrollment(runner=self.run_response([], change)).verify_factory(self.dut())

    def test_invalid_reply_and_timeout_fail_once_and_never_expose_captured_sdk_output(self):
        for mutate in (lambda r: r.update(device_id="b" * 32), lambda r: r.update(node_id=True),
                       lambda r: r.update(request_id="x"), lambda r: r.update(commissioned=False)):
            calls = []
            with self.subTest(mutate=mutate), self.assertRaises(EnrollmentError):
                MatterEnrollment(runner=self.run_response(calls, mutate)).commission(self.dut())
            self.assertEqual(len(calls), 1)
        calls = []
        failure = subprocess.TimeoutExpired(["private command"], 30, output="private wifi password")
        with self.assertRaises(EnrollmentError) as error:
            MatterEnrollment(runner=self.run_response(calls, failure=failure)).commission(self.dut())
        self.assertNotIn("private wifi password", str(error.exception))
        self.assertIsNone(error.exception.__cause__)

    def test_reachable_main_fails_even_if_configuration_not_ready(self):
        dut = self.dut({"token": "session"}, {**self.system, "ready": False}, self.system)
        commissioner = Mock()
        with self.assertRaisesRegex(EnrollmentError, "must be factory reset"):
            ensure_enrolled(dut, commissioner)
        self.assertFalse(dut.requests)
        commissioner.commission.assert_not_called()
        self.assertFalse(dut.presses)

    def test_online_main_never_attempts_expensive_login(self):
        dut, commissioner = self.dut(TimeoutError("password response timed out")), Mock()
        # The readiness request itself succeeds; only password verification is slow.
        dut._opener = SimpleNamespace(open=lambda request, timeout: io.BytesIO(
            json.dumps({"device_id": DEVICE_ID, "ready": True}).encode()))
        with self.assertRaisesRegex(EnrollmentError, "must be factory reset"):
            ensure_enrolled(dut, commissioner)
        self.assertFalse(dut.requests)
        commissioner.commission.assert_not_called()
        self.assertFalse(dut.presses)

    def factory_controller(self, fabrics=0, networks=None):
        class Operational:
            Attributes = SimpleNamespace(CommissionedFabrics=object())

        class Network:
            Attributes = SimpleNamespace(Networks=object())

        commissioning = SimpleNamespace(
            Commands=SimpleNamespace(ArmFailSafe=lambda **fields: SimpleNamespace(**fields)),
            Enums=SimpleNamespace(CommissioningErrorEnum=SimpleNamespace(kOk=0)))
        clusters = SimpleNamespace(OperationalCredentials=Operational, NetworkCommissioning=Network,
                                   GeneralCommissioning=commissioning)
        calls = []

        async def pase(*args):
            calls.append(("pase", args))

        async def read(*args):
            calls.append(("read", args))
            return {0: {Operational: {Operational.Attributes.CommissionedFabrics: fabrics},
                        Network: {Network.Attributes.Networks: [] if networks is None else networks}}}

        async def commission_wifi(*args):
            calls.append(("commission_wifi", args))
            return 1

        async def send(*args):
            calls.append(("send", args))
            return SimpleNamespace(errorCode=0)

        return SimpleNamespace(EstablishPASESessionBLE=pase, ReadAttribute=read,
                               CommissionWiFi=commission_wifi, SendCommand=send), clusters, calls

    def test_native_factory_precondition_precedes_any_wifi_or_fabric_provisioning(self):
        controller, clusters, calls = self.factory_controller()
        asyncio.run(_commission_factory(controller, clusters, CREDENTIALS, timeout=30))
        self.assertEqual([call[0] for call in calls], ["pase", "read", "commission_wifi"])
        self.assertEqual(calls[0][1], (20202021, 3840, 1))
        self.assertEqual(calls[2][1], (3840, 20202021, 1,
                                     CREDENTIALS["wifi_ssid"], CREDENTIALS["wifi_password"]))

    def test_native_commission_requires_sdk_confirmation_for_assigned_node(self):
        for node in (None, True, 2):
            controller, clusters, calls = self.factory_controller()

            async def commission_wifi(*args):
                calls.append(("commission_wifi", args))
                return node

            controller.CommissionWiFi = commission_wifi
            with self.subTest(node=node), self.assertRaisesRegex(EnrollmentError, "assigned node"):
                asyncio.run(_commission_factory(controller, clusters, CREDENTIALS, timeout=30))

    def test_native_verify_factory_stops_after_pase_and_standard_attribute_reads(self):
        controller, clusters, calls = self.factory_controller()
        asyncio.run(_verify_factory(controller, clusters, CREDENTIALS, timeout=30))
        self.assertEqual([call[0] for call in calls], ["pase", "read"])

    def test_factory_verification_releases_its_failsafe_once_after_proving_empty_state(self):
        controller, clusters, calls = self.factory_controller()
        asyncio.run(_verify_factory_and_release(controller, clusters, CREDENTIALS, timeout=30))
        self.assertEqual([call[0] for call in calls], ["pase", "read", "send"])
        node, endpoint, command = calls[-1][1]
        self.assertEqual((node, endpoint), (1, 0))
        self.assertEqual(vars(command), {"expiryLengthSeconds": 0, "breadcrumb": 0})

    def test_failed_factory_verification_never_sends_failsafe_cleanup(self):
        for fabrics, networks in ((1, []), (0, [{"networkID": b"old"}]), (False, []), (0, "")):
            with self.subTest(fabrics=fabrics, networks=networks):
                controller, clusters, calls = self.factory_controller(fabrics, networks)
                with self.assertRaisesRegex(EnrollmentError, "must be factory reset"):
                    asyncio.run(_verify_factory_and_release(controller, clusters, CREDENTIALS, timeout=30))
                self.assertEqual([call[0] for call in calls], ["pase", "read"])

    def test_factory_failsafe_cleanup_requires_an_explicit_success_response(self):
        for response in (None, SimpleNamespace(), SimpleNamespace(errorCode=1),
                         SimpleNamespace(errorCode=False), SimpleNamespace(errorCode=0.0)):
            with self.subTest(response=response):
                controller, clusters, calls = self.factory_controller()
                controller.SendCommand = AsyncMock(return_value=response)
                with self.assertRaisesRegex(EnrollmentError, "did not release"):
                    asyncio.run(_verify_factory_and_release(controller, clusters, CREDENTIALS, timeout=30))
                controller.SendCommand.assert_awaited_once()
                self.assertEqual([call[0] for call in calls], ["pase", "read"])

    def test_factory_failsafe_cleanup_failure_is_not_retried(self):
        controller, clusters, calls = self.factory_controller()
        controller.SendCommand = AsyncMock(side_effect=RuntimeError("command failed"))
        with self.assertRaisesRegex(RuntimeError, "command failed"):
            asyncio.run(_verify_factory_and_release(controller, clusters, CREDENTIALS, timeout=30))
        controller.SendCommand.assert_awaited_once()

    def test_factory_failsafe_cleanup_shares_the_verification_deadline(self):
        controller, clusters, calls = self.factory_controller()
        now = [0]
        read = controller.ReadAttribute

        async def slow_read(*args):
            result = await read(*args)
            now[0] = 31
            return result

        controller.ReadAttribute = slow_read
        with patch("amity_harness.enrollment.time.monotonic", side_effect=lambda: now[0]):
            with self.assertRaisesRegex(TimeoutError, "deadline expired"):
                asyncio.run(_verify_factory_and_release(controller, clusters, CREDENTIALS, timeout=30))
        self.assertEqual([call[0] for call in calls], ["pase", "read"])

    def test_native_rejects_saved_fabric_or_network_before_provisioning(self):
        for fabrics, networks in ((1, []), (0, [{"networkID": b"old"}]), (False, []), (0, "")):
            with self.subTest(fabrics=fabrics, networks=networks):
                controller, clusters, calls = self.factory_controller(fabrics, networks)
                with self.assertRaisesRegex(EnrollmentError, "must be factory reset"):
                    asyncio.run(_commission_factory(controller, clusters, CREDENTIALS, timeout=30))
                self.assertEqual([call[0] for call in calls], ["pase", "read"])

    def test_native_missing_factory_attributes_fail_closed(self):
        controller, clusters, calls = self.factory_controller()

        async def read(*args):
            calls.append(("read", args))
            return {}

        controller.ReadAttribute = read
        with self.assertRaisesRegex(EnrollmentError, "could not verify"):
            asyncio.run(_commission_factory(controller, clusters, CREDENTIALS, timeout=30))
        self.assertEqual([call[0] for call in calls], ["pase", "read"])

    def test_worker_isolates_platform_and_uses_new_generation_after_reset(self):
        sdk, paa = self.root / "sdk", self.root / "paa"
        sdk.mkdir(); paa.mkdir()
        args = SimpleNamespace(storage=str(self.root / "matter"), sdk_path=str(sdk), paa_path=str(paa),
                               timeout=30, ble_controller=0)
        request = {"protocol_version": 1, "request_id": "1" * 32, "operation": "enroll",
                   "device_id": DEVICE_ID, "credentials": CREDENTIALS}
        generations, calls = [], []

        def native(command, **options):
            calls.append((command, options))
            generations.append(Path(command[command.index("--storage") + 1]))
            self.assertEqual(command[:7], ["sudo", "-n", "unshare", "--mount", "--propagation", "private", "env"])
            self.assertEqual(json.loads(options["input"])["credentials"], CREDENTIALS)
            return SimpleNamespace(returncode=0)

        self.assertTrue(_worker(request, args, runner=native)["commissioned"])
        self.assertTrue(_worker(request, args, runner=native)["commissioned"])
        self.assertNotEqual(generations[0], generations[1])
        self.assertFalse(generations[0].exists())
        self.assertEqual((generations[1].parent / "current").resolve(), generations[1])
        self.assertTrue((generations[1] / "platform").is_dir())
        self.assertEqual(stat.S_IMODE(generations[1].stat().st_mode), 0o700)
        self.assertNotIn(CREDENTIALS["wifi_password"], str(calls[0][0]))
        request["operation"] = "verify_factory"
        self.assertTrue(_worker(request, args, runner=native)["factory_reset"])
        self.assertTrue(generations[1].exists())
        self.assertEqual((generations[1].parent / "current").resolve(), generations[1])
        self.assertEqual((generations[2].parent / "last-verification").resolve(), generations[2])
        self.assertTrue(generations[2].name.startswith("verification-"))

    def test_worker_rejects_bad_request_identifiers_before_native_work(self):
        good = {"protocol_version": 1, "request_id": "1" * 32, "operation": "enroll",
                "device_id": DEVICE_ID, "credentials": None}
        self.assertEqual(_request(io.StringIO(json.dumps(good))), good)
        for change in ({"device_id": "../escape"}, {"protocol_version": True}, {"request_id": "bad"},
                       {"operation": "reset"}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                _request(io.StringIO(json.dumps({**good, **change})))


if __name__ == "__main__":
    unittest.main()
