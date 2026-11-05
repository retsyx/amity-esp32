"""Provision the bench main through standard Matter BLE Wi-Fi commissioning.

Only this bootstrap uses Matter. After enrollment, test control and observations
continue through the shipping authenticated HTTPS UI. Secrets pass to the local
worker on stdin; its private log and isolated storage remain on the Pi.
"""

import argparse
import asyncio
from contextlib import contextmanager
import ctypes
import errno
import fcntl
import http.client
import json
import logging
import os
from pathlib import Path
import pwd
import re
import shutil
import signal
import socket
import ssl
import stat
import subprocess
import sys
import tempfile
import time
import traceback
from urllib.error import URLError
from urllib.request import Request
from uuid import uuid4

from .dut import DUT, _positive_seconds
from .trace import event


DEFAULT_CREDENTIALS = "~/.config/amity-harness/enrollment.json"
DEFAULT_STORAGE = "~/.local/state/amity-harness/matter"
_PROTOCOL_VERSION = 1
_MAX_REQUEST = 16384
_CLEANUP_ALLOWANCE = 10
_TERMINATE_GRACE = 2
_KILL_GRACE = 2
_CREDENTIAL_FIELDS = {"wifi_ssid", "wifi_password", "matter_pin_code", "matter_discriminator"}


class EnrollmentError(RuntimeError):
    """A sanitized bootstrap failure; native details stay in the Pi private log."""


class _WorkerInterrupted(EnrollmentError):
    """Avoid OSError subclasses, which subprocess/select may consume as EINTR."""


def _credentials(value):
    if not isinstance(value, dict) or set(value) != _CREDENTIAL_FIELDS:
        raise ValueError("Enrollment credentials require Wi-Fi SSID/password and Matter PIN/discriminator")
    for field, maximum, empty in (("wifi_ssid", 32, False), ("wifi_password", 64, True)):
        item = value[field]
        if (not isinstance(item, str) or "\x00" in item or len(item.encode("utf-8")) > maximum
                or (not empty and not item)):
            raise ValueError("Invalid enrollment Wi-Fi credentials")
    pin, discriminator = value["matter_pin_code"], value["matter_discriminator"]
    if (type(pin) is not int or not 1 <= pin <= 99999998
            or pin in (11111111, 22222222, 33333333, 44444444, 55555555,
                       66666666, 77777777, 88888888, 12345678, 87654321)
            or type(discriminator) is not int or not 0 <= discriminator <= 4095):
        raise ValueError("Invalid enrollment Matter PIN or discriminator")
    return dict(value)


def _read_credentials(path):
    path = Path(path).expanduser()
    if stat.S_IMODE(path.stat().st_mode) & 0o077:
        raise ValueError("Enrollment credentials must be private (mode 0600)")
    if path.stat().st_size > _MAX_REQUEST:
        raise ValueError("Enrollment credential file is too large")
    return _credentials(json.loads(path.read_text(encoding="utf-8")))


def _text(value, description):
    if not isinstance(value, str) or not value or "\x00" in value:
        raise ValueError("Invalid " + description)
    return value


class MatterEnrollment:
    """One bounded enrollment through a short-lived worker on the harness Pi.

    ``credentials=None`` reads the Pi's private default enrollment file. An
    explicit private file is passed to the local worker only on stdin.
    Each enrollment uses fresh controller/platform state, avoiding stale fabric
    records after a factory reset. The latest successful generation is retained.
    """

    def __init__(self, *, credentials=None,
                 sdk_path=None, paa_path=None, storage=DEFAULT_STORAGE,
                 timeout=180, python=sys.executable, ble_controller=0, runner=None):
        _positive_seconds(timeout, "Matter enrollment timeout")
        if type(ble_controller) is not int or not 0 <= ble_controller <= 255:
            raise ValueError("Invalid enrollment Bluetooth adapter")
        self.credentials = None if credentials is None else Path(credentials)
        self.sdk_path = None if sdk_path is None else _text(str(sdk_path), "Matter SDK path")
        self.paa_path = None if paa_path is None else _text(str(paa_path), "Matter PAA path")
        self.storage = _text(str(storage), "Matter storage path")
        self.python = _text(python, "enrollment Python executable")
        self.timeout, self.ble_controller = timeout, ble_controller
        self.runner = runner or _run_process_group

    def _command(self, timeout=None):
        timeout = self.timeout if timeout is None else timeout
        arguments = [self.python, "-u", "-m", "amity_harness.enrollment", "--worker",
                     "--timeout", str(timeout), "--storage", self.storage,
                     "--ble-controller", str(self.ble_controller)]
        if self.sdk_path is not None:
            arguments += ["--sdk-path", self.sdk_path]
        if self.paa_path is not None:
            arguments += ["--paa-path", self.paa_path]
        return arguments

    def commission(self, dut, *, timeout=None):
        return self._operation(dut, "enroll", timeout=timeout)

    def verify_factory(self, dut, *, timeout=None):
        """Prove zero saved fabrics/networks and release the verification PASE."""
        return self._operation(dut, "verify_factory", timeout=timeout)

    def _operation(self, dut, operation, *, timeout=None):
        timeout = self.timeout if timeout is None else min(timeout, self.timeout)
        _positive_seconds(timeout, "Matter enrollment timeout")
        if dut.name != dut._runtime.main_dut:
            raise ValueError("Matter bootstrap enrolls only the selected bench main")
        request_id = uuid4().hex
        request = {"protocol_version": _PROTOCOL_VERSION, "request_id": request_id, "operation": operation,
                   "device_id": dut.identity.device_id,
                   "credentials": None if self.credentials is None else _read_credentials(self.credentials)}
        event_name = "commission" if operation == "enroll" else "verify_factory"
        event("enrollment." + event_name + ".start", dut=dut.name, device_id=dut.identity.device_id,
              timeout_seconds=timeout)
        environment = {**os.environ, "PYTHONPATH": str(Path(__file__).resolve().parents[1])}
        try:
            response = self.runner(self._command(timeout), input=json.dumps(request), text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   timeout=timeout + _CLEANUP_ALLOWANCE, env=environment, check=False)
            if response.returncode != 0 or len(response.stdout) > _MAX_REQUEST:
                raise EnrollmentError("Matter enrollment worker failed; inspect its private Pi commissioning log")
            reply = json.loads(response.stdout)
            if (not isinstance(reply, dict) or reply.get("protocol_version") != _PROTOCOL_VERSION
                    or type(reply.get("protocol_version")) is not int
                    or reply.get("request_id") != request_id or reply.get("device_id") != dut.identity.device_id
                    or reply.get("operation") != operation):
                raise EnrollmentError("Matter worker did not confirm the requested operation")
            if operation == "enroll":
                if (reply.get("commissioned") is not True or reply.get("node_id") != 1
                        or type(reply.get("node_id")) is not int):
                    raise EnrollmentError("Matter enrollment worker did not confirm commissioning")
                result = {"commissioned": True, "node_id": 1, "device_id": dut.identity.device_id}
            else:
                if reply.get("factory_reset") is not True:
                    raise EnrollmentError("Matter worker did not verify factory reset")
                result = {"factory_reset": True, "device_id": dut.identity.device_id}
        except (KeyboardInterrupt, SystemExit):
            raise
        except Exception as error:
            event("enrollment." + event_name + ".failed", level=logging.ERROR, dut=dut.name,
                  error_type=type(error).__name__)
            raise EnrollmentError("Matter enrollment failed; inspect the Pi's private Matter storage log") from None
        event("enrollment." + event_name + ".complete", dut=dut.name, **result)
        return result


def _unreachable(error):
    # A reachable device rejecting authentication, TLS or identity must fail;
    # it must never be treated as an uncommissioned board.
    if isinstance(error, ssl.SSLError):
        return False
    if isinstance(error, URLError):
        return isinstance(error.reason, BaseException) and _unreachable(error.reason)
    return (isinstance(error, (ConnectionError, TimeoutError, socket.gaierror, http.client.RemoteDisconnected))
            or isinstance(error, OSError) and error.errno in {
                errno.ENETUNREACH, errno.EHOSTUNREACH, errno.ETIMEDOUT, errno.ECONNREFUSED,
                errno.ECONNRESET, errno.ENETDOWN, errno.EHOSTDOWN, errno.EADDRNOTAVAIL})


def _authenticated_system(dut, password, timeout):
    deadline = time.monotonic() + timeout
    try:
        if not dut._token:
            reply = dut.request("/api/login", {"password": password}, timeout=timeout)
            token = reply.get("token") if isinstance(reply, dict) else None
            if not isinstance(token, str) or not token:
                raise EnrollmentError("DUT login did not return an authenticated session")
            dut._token = token
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("DUT authenticated system lookup deadline expired")
        system = dut.request("/api/system", timeout=remaining)
        if not isinstance(system, dict) or system.get("octopus_id") != dut.identity.device_id:
            raise AssertionError("Enrolled DUT HTTP identity does not match TEST-HARNESS.md")
        return system
    except BaseException:
        dut._token = None
        dut.close()
        raise


def _probe_ready(dut, *, timeout):
    # The shipping UI uses this credential-free route when following a renamed
    # host. Probe it before expensive password hashing: a slow login response
    # from an online main must never trigger BLE commissioning.
    try:
        with dut._opener.open(Request(dut.url + "/api/ready", headers={"Accept": "application/json"}),
                              timeout=timeout) as response:
            ready = json.load(response)
        if not isinstance(ready, dict) or ready.get("device_id") != dut.identity.device_id:
            raise AssertionError("DUT readiness identity does not match TEST-HARNESS.md")
    except BaseException:
        dut.close()
        raise


def ensure_enrolled(dut, commissioner, *, timeout=180):
    """Require a factory-reset main and commission it before scenario fixtures.

    A short physical setup-button press reopens the initial Matter window even
    when the automatic boot window has expired. It does not erase DUT settings.
    Factory reset remains an explicit caller action.
    """
    _positive_seconds(timeout, "Enrollment readiness timeout")
    deadline = time.monotonic() + timeout
    if dut.name != dut._runtime.main_dut:
        raise ValueError("Matter bootstrap prepares only the selected bench main")
    credential_path = dut._runtime.credentials_for(dut.name)
    if credential_path is None:
        raise ValueError("Provide the bench main's initial UI credentials")
    password = DUT._read_password(credential_path)
    event("enrollment.prepare.start", dut=dut.name, device_id=dut.identity.device_id)
    try:
        _probe_ready(dut, timeout=min(15, timeout))
    except Exception as error:
        if not _unreachable(error):
            raise
        dut.press_fw_reset(0.5)
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Matter enrollment preparation deadline expired")
        commissioner.commission(dut, timeout=remaining)
        system = None
    else:
        dut.close()
        raise EnrollmentError("The selected bench main must be factory reset before running functional tests")
    while True:
        if system is not None:
            membership = system.get("membership", {})
            if (system.get("mode") != "coordinator" or not isinstance(membership, dict)
                    or membership.get("coordinator") is not True or membership.get("member") is not False):
                raise AssertionError("Bench main is not the original Amity controller")
            if (system.get("ready") is True and system.get("configuration_pending") is False
                    and membership.get("authority_ready") is True):
                break
            time.sleep(min(0.5, max(0, deadline - time.monotonic())))
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Commissioned DUT did not become ready for authenticated HTTPS")
        try:
            system = _authenticated_system(dut, password, min(15, remaining))
        except Exception as error:
            if not _unreachable(error):
                raise
            system = None
            time.sleep(min(0.5, max(0, deadline - time.monotonic())))
    event("enrollment.prepare.complete", dut=dut.name, device_id=dut.identity.device_id,
          boot_id=system.get("boot_id"))
    return {"enrolled": True, "device_id": dut.identity.device_id, "boot_id": system.get("boot_id")}


@contextmanager
def _adapter_lock(adapter):
    path = Path(tempfile.gettempdir()) / f"amity-ble-hci{adapter}.lock"
    descriptor = os.open(path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield
    finally:
        os.close(descriptor)


def _signal_group(group, signum):
    try:
        os.killpg(group, signum)
    except ProcessLookupError:
        pass
    except PermissionError:
        # The launcher can still be running as root before dropping to the Pi
        # user. Use the same noninteractive sudo authority as the launcher.
        result = subprocess.run(["sudo", "-n", "kill", "-" + signal.Signals(signum).name[3:],
                                 "--", "-" + str(group)], stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL, timeout=_KILL_GRACE, check=False)
        if result.returncode != 0:
            try:
                os.killpg(group, 0)
            except ProcessLookupError:
                return
            raise EnrollmentError("Could not stop the Matter worker process group") from None


def _stop_process_group(process):
    # A second interruption must not bypass cleanup while a native controller
    # still owns the Bluetooth adapter. The lock stays held until this returns.
    previous = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGINT, signal.SIGHUP, signal.SIGTERM})
    try:
        _signal_group(process.pid, signal.SIGTERM)
        deadline = time.monotonic() + _TERMINATE_GRACE
        while time.monotonic() < deadline:
            process.poll()
            try:
                os.killpg(process.pid, 0)
            except ProcessLookupError:
                break
            time.sleep(0.05)
        # The direct child can exit while a descendant ignores TERM. Always
        # stop the remaining group before reaping the launcher.
        _signal_group(process.pid, signal.SIGKILL)
        process.wait(timeout=_KILL_GRACE)
    finally:
        signal.pthread_sigmask(signal.SIG_SETMASK, previous)


def _run_process_group(command, *, input=None, timeout=None, check=False, **options):
    process = subprocess.Popen(command, stdin=subprocess.PIPE, start_new_session=True, **options)
    try:
        stdout, stderr = process.communicate(input=input, timeout=timeout)
    except BaseException:
        _stop_process_group(process)
        raise
    result = subprocess.CompletedProcess(command, process.returncode, stdout, stderr)
    if check:
        result.check_returncode()
    return result


@contextmanager
def _worker_signals():
    previous = {}

    def interrupted(signum, frame):
        raise _WorkerInterrupted("Matter worker interrupted by " + signal.Signals(signum).name)

    try:
        for signum in (signal.SIGINT, signal.SIGHUP, signal.SIGTERM):
            previous[signum] = signal.signal(signum, interrupted)
        if sys.platform.startswith("linux"):
            # Stop the worker if the runner exits before normal cleanup.
            parent = os.getppid()
            libc = ctypes.CDLL(None, use_errno=True)
            if libc.prctl(1, int(signal.SIGHUP), 0, 0, 0) != 0:  # PR_SET_PDEATHSIG
                raise OSError(ctypes.get_errno(), "Cannot watch the Matter worker parent")
            if parent == 1 or os.getppid() != parent:
                raise _WorkerInterrupted("Matter worker parent already exited")
        yield
    finally:
        for signum, handler in previous.items():
            signal.signal(signum, handler)


def _worker(request, args, *, runner=None):
    credentials = (_read_credentials(DEFAULT_CREDENTIALS) if request["credentials"] is None
                   else _credentials(request["credentials"]))
    storage = Path(args.storage).expanduser().resolve()
    storage.mkdir(parents=True, exist_ok=True, mode=0o700)
    if stat.S_IMODE(storage.stat().st_mode) & 0o077:
        raise ValueError("Matter controller storage must be private")
    owner = storage / request["device_id"]
    owner.mkdir(exist_ok=True, mode=0o700)
    prefix = "generation-" if request["operation"] == "enroll" else "verification-"
    generation = Path(tempfile.mkdtemp(prefix=prefix, dir=owner))
    (generation / "platform").mkdir(mode=0o700)
    project = Path(__file__).resolve().parents[2]
    sdk = Path(args.sdk_path).expanduser().resolve() if args.sdk_path else project / ".matter-python"
    paa = Path(args.paa_path).expanduser().resolve() if args.paa_path else project / "paa-root-certs"
    if not sdk.is_dir() or not paa.is_dir():
        raise EnrollmentError("Install the Pi Matter Python SDK and PAA trust store before enrollment")
    command = ["sudo", "-n", "unshare", "--mount", "--propagation", "private", "env",
               "PYTHONPATH=" + str(Path(__file__).resolve().parents[1]), sys.executable,
               "-u", "-m", "amity_harness.enrollment", "--native-worker",
               "--storage", str(generation), "--sdk-path", str(sdk), "--paa-path", str(paa),
               "--timeout", str(args.timeout), "--ble-controller", str(args.ble_controller)]
    native_request = {**request, "credentials": credentials}
    with _adapter_lock(args.ble_controller), (generation / "commissioning.log").open("w") as log:
        result = (runner or _run_process_group)(command, input=json.dumps(native_request), text=True,
                        stdout=log, stderr=subprocess.STDOUT, check=False, timeout=args.timeout)
        if result.returncode != 0:
            raise EnrollmentError("Native Matter commissioning failed; inspect its private generation log")
    current_name = "current" if request["operation"] == "enroll" else "last-verification"
    current = owner / ("." + current_name + "-" + uuid4().hex)
    current.symlink_to(generation.name)
    current.replace(owner / current_name)
    for previous in owner.glob(prefix + "*"):
        if previous != generation and previous.is_dir() and not previous.is_symlink():
            shutil.rmtree(previous)
    result = ({"commissioned": True, "node_id": 1} if request["operation"] == "enroll"
              else {"factory_reset": True})
    return {"protocol_version": _PROTOCOL_VERSION, "request_id": request["request_id"],
            "device_id": request["device_id"], "operation": request["operation"], **result}


async def _bounded(operation, deadline):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        operation.close()
        raise TimeoutError("Matter factory enrollment deadline expired")
    return await asyncio.wait_for(operation, remaining)


async def _verify_factory(controller, clusters, credentials, *, timeout):
    """Read standard commissioning state over PASE without changing networks."""
    deadline = time.monotonic() + timeout
    await _bounded(controller.EstablishPASESessionBLE(
        credentials["matter_pin_code"], credentials["matter_discriminator"], 1), deadline)
    operational = clusters.OperationalCredentials
    network = clusters.NetworkCommissioning
    attributes = await _bounded(controller.ReadAttribute(1, [
        (0, operational.Attributes.CommissionedFabrics), (0, network.Attributes.Networks)]), deadline)
    try:
        fabrics = attributes[0][operational][operational.Attributes.CommissionedFabrics]
        networks = attributes[0][network][network.Attributes.Networks]
    except (KeyError, TypeError):
        raise EnrollmentError("Matter could not verify that the selected DUT is factory reset") from None
    if (not isinstance(fabrics, int) or isinstance(fabrics, bool) or fabrics != 0
            or not isinstance(networks, list) or networks):
        raise EnrollmentError("The selected DUT must be factory reset; saved Matter fabrics or networks remain")


async def _commission_factory(controller, clusters, credentials, *, timeout):
    """Reject saved fabrics/networks over PASE before supplying Wi-Fi settings."""
    deadline = time.monotonic() + timeout
    await _verify_factory(controller, clusters, credentials, timeout=timeout)
    # The Python SDK's bare Commission() starts with empty network parameters.
    # CommissionWiFi supplies them and reuses this same node's established PASE.
    node = await _bounded(controller.CommissionWiFi(
        credentials["matter_discriminator"], credentials["matter_pin_code"], 1,
        credentials["wifi_ssid"], credentials["wifi_password"]), deadline)
    if not isinstance(node, int) or isinstance(node, bool) or node != 1:
        raise EnrollmentError("Matter controller did not confirm the assigned node")


async def _verify_factory_and_release(controller, clusters, credentials, *, timeout):
    """Verify factory state and release this verification's commissioning fail-safe."""
    deadline = time.monotonic() + timeout
    await _verify_factory(controller, clusters, credentials, timeout=timeout)
    # PASE arms the DUT's fail-safe even for reads. BLE disconnect alone does
    # not release it, so cancel it through the standard commissioning command
    # after proving that no saved fabrics or networks can be rolled back.
    commissioning = clusters.GeneralCommissioning
    response = await _bounded(controller.SendCommand(
        1, 0, commissioning.Commands.ArmFailSafe(expiryLengthSeconds=0, breadcrumb=0)), deadline)
    error = getattr(response, "errorCode", None)
    if not isinstance(error, int) or isinstance(error, bool) or error != commissioning.Enums.CommissioningErrorEnum.kOk:
        raise EnrollmentError("Matter did not release the factory verification fail-safe")


def _native_worker(request, args):
    storage = Path(args.storage)
    # CHIP's Linux platform filenames live under /tmp in the pinned Pi SDK.
    # Bind only in a private mount namespace; never touch global platform data.
    subprocess.run(["mount", "--bind", str(storage / "platform"), "/tmp"], check=True)
    if os.geteuid() == 0 and "SUDO_UID" in os.environ and "SUDO_GID" in os.environ:
        uid, gid = int(os.environ["SUDO_UID"]), int(os.environ["SUDO_GID"])
        os.initgroups(pwd.getpwuid(uid).pw_name, gid)
        os.setgid(gid)
        os.setuid(uid)
    sys.path.insert(0, str(Path(args.sdk_path)))
    import chip.native
    import chip.clusters as Clusters
    from chip.ChipStack import ChipStack
    from chip.CertificateAuthority import CertificateAuthorityManager

    stack = manager = controller = None
    try:
        chip.native.Init(bluetoothAdapter=args.ble_controller)
        stack = ChipStack(str(storage / "controller.json"))
        manager = CertificateAuthorityManager(stack)
        manager.LoadAuthoritiesFromStorage()
        authority = manager.activeCaList[0] if manager.activeCaList else manager.NewCertificateAuthority(caIndex=1)
        admin = authority.adminList[0] if authority.adminList else authority.NewFabricAdmin(vendorId=0xFFF1, fabricId=1)
        controller = admin.NewController(nodeId=112233, paaTrustStorePath=str(args.paa_path))
        credentials = _credentials(request["credentials"])

        operation = _commission_factory if request["operation"] == "enroll" else _verify_factory_and_release
        asyncio.run(operation(controller, Clusters, credentials, timeout=args.timeout))
    finally:
        if controller is not None:
            controller.Shutdown()
        if manager is not None:
            manager.Shutdown()
        if stack is not None:
            stack.Shutdown()


def _request(stream):
    value = json.loads(stream.read(_MAX_REQUEST + 1))
    if (not isinstance(value, dict) or set(value) != {"protocol_version", "request_id", "device_id", "credentials", "operation"}
            or type(value["protocol_version"]) is not int or value["protocol_version"] != _PROTOCOL_VERSION
            or value["operation"] not in ("enroll", "verify_factory")
            or not isinstance(value["request_id"], str) or re.fullmatch(r"[0-9a-f]{32}", value["request_id"]) is None
            or not isinstance(value["device_id"], str) or re.fullmatch(r"[0-9a-f]{32}", value["device_id"]) is None):
        raise ValueError("Invalid Matter enrollment request")
    return value


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--worker", action="store_true")
    modes.add_argument("--native-worker", action="store_true")
    parser.add_argument("--storage", default=DEFAULT_STORAGE)
    parser.add_argument("--sdk-path")
    parser.add_argument("--paa-path")
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--ble-controller", type=int, default=0)
    args = parser.parse_args(argv)
    os.umask(0o077)
    try:
        _positive_seconds(args.timeout, "Matter enrollment timeout")
        if not 0 <= args.ble_controller <= 255:
            raise ValueError("Invalid Matter Bluetooth adapter")
        request = _request(sys.stdin)
        if args.native_worker:
            _native_worker(request, args)
        else:
            with _worker_signals():
                print(json.dumps(_worker(request, args)), flush=True)
        return 0
    except Exception as error:
        # SDK output can contain setup material. Never forward exception text.
        if args.native_worker:
            # Both native streams are already redirected to the Pi private log.
            traceback.print_exc()
        print(json.dumps({"commissioned": False, "error_type": type(error).__name__}), flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
