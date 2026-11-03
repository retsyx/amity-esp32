"""Shared functional-test CLI, runtime configuration and private reporting."""

import argparse
from contextlib import nullcontext
from functools import partial
import json
import logging
import math
import os
from pathlib import Path
import sys
import time

from .cec import CecSession
from .config import RunConfig, current_config, track_testbeds, use_config
from .status_led import run_status_led
from .trace import event, failure_fields, stdio_logging, test_logging


def run_test(test, config):
    """Execute a scenario; resource contexts own cleanup before reporting."""
    report = {"test": test.__name__, "passed": False}
    started = time.monotonic()
    with test_logging(test.__name__), use_config(config), track_testbeds() as beds:
        event("test.start")
        try:
            test()
            report["passed"] = True
        except Exception as exc:
            details = failure_fields(exc)
            report["error"] = details["error_type"] + (": " + details["message"] if "message" in details else "")
            report["error_type"] = type(exc).__name__
            report["failure"] = details
            event("test.failed", level=logging.ERROR, **details)
        except BaseException as exc:
            event("test.interrupted", level=logging.ERROR, **failure_fields(exc))
            raise
        finally:
            report["testbeds"] = [bed.report for bed in beds]
            event("test.finish", passed=report["passed"],
                  duration_ms=round((time.monotonic() - started) * 1000, 3),
                  testbeds=len(beds))
    return report


def _write_report(path, report):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as output:
        os.fchmod(output.fileno(), 0o600)
        json.dump(report, output, indent=2)
        output.write("\n")


def _named_options(parser, values, description, convert=str):
    options = {}
    for value in values:
        name, separator, setting = value.partition("=")
        if not separator or not name or not setting or name in options:
            parser.error(f"Use unique DUT={description} entries")
        options[name] = convert(setting)
    return options


def prepare_main():
    """Enroll the bench main before any firmware or functional scenario."""
    from .dut import DUT
    from .enrollment import MatterEnrollment, ensure_enrolled

    config = current_config()
    dut = DUT("amity-dut")
    try:
        commissioner = (config.enrollment_factory or MatterEnrollment)()
        ensure_enrolled(dut, commissioner, timeout=config.enrollment_timeout)
    finally:
        dut.logout()


def reset_main():
    """Reset a successfully tested main and verify its factory Matter state."""
    from .dut import DUT
    from .enrollment import MatterEnrollment, _probe_ready, _unreachable

    config = current_config()
    dut = DUT("amity-dut")
    deadline = time.monotonic() + config.enrollment_timeout
    try:
        dut.authenticate()
        system = dut.get_system()
        if system.get("octopus_id") != dut.identity.device_id:
            raise AssertionError("DUT HTTP identity does not match amity-dut in TEST-HARNESS.md")
        response = dut.request("/api/factory-reset", {"confirm": True})
        if response.get("rebooting") is not True:
            raise RuntimeError("The main did not acknowledge its factory reset")
        event("enrollment.reset.scheduled", dut=dut.name, device_id=dut.identity.device_id)
        dut._token = None
        dut.close()
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("The main did not finish its factory reset")
            try:
                _probe_ready(dut, timeout=min(3, remaining))
            except Exception as error:
                if not _unreachable(error):
                    raise
                break
            time.sleep(min(0.5, remaining))
        commissioner = (config.enrollment_factory or MatterEnrollment)()
        commissioner.verify_factory(dut, timeout=deadline - time.monotonic())
        event("enrollment.reset.complete", dut=dut.name, device_id=dut.identity.device_id)
    finally:
        dut._token = None
        dut.close()


def main(test, argv=None, *, firmware_upgrade=None, prepare=None, finish=None):
    """Run required preparation, optional firmware upgrade, then scenarios."""
    if firmware_upgrade is not None and not callable(firmware_upgrade):
        raise ValueError("firmware_upgrade must be callable or None")
    if prepare is not None and not callable(prepare):
        raise ValueError("prepare must be callable or None")
    if finish is not None and (not callable(finish) or prepare is None):
        raise ValueError("finish requires a prepare callback and must be callable")
    tests = {test.__name__: test} if callable(test) else dict(test)
    if not tests or any(not isinstance(name, str) for name in tests):
        raise ValueError("Provide named functional tests or nonempty groups of callable tests")
    for name, group in tests.items():
        if callable(group):
            tests[name] = (group,)
        elif isinstance(group, (tuple, list)) and group and all(callable(scenario) for scenario in group):
            tests[name] = tuple(group)
        else:
            raise ValueError("Provide named functional tests or nonempty groups of callable tests")
    parser = argparse.ArgumentParser(description="Run Amity functional tests using the CEC harness")
    if "firmware_upgrade" in tests:
        raise ValueError("Register firmware_upgrade using the dedicated callback parameter")
    if "enrollment" in tests:
        raise ValueError("Register enrollment using the prepare callback parameter")
    choices = [*tests, *(('firmware_upgrade',) if firmware_upgrade is not None else ()),
               *(('enrollment',) if prepare is not None else ()), "all"]
    parser.add_argument("--test", choices=choices, default=next(iter(tests)),
                        help="scenario to run; defaults to the first registered test")
    parser.add_argument("--credentials", type=Path, help="private JSON with an existing DUT admin_password")
    parser.add_argument("--dut-credentials", action="append", default=[], metavar="DUT=PATH",
                        help="per-DUT private credentials; repeat for multiple DUTs")
    parser.add_argument("--url", help="override the default DUT HTTPS origin")
    parser.add_argument("--dut-url", action="append", default=[], metavar="DUT=URL",
                        help="per-DUT HTTPS origin; repeat for multiple DUTs")
    parser.add_argument("--wiring", type=Path, help="override the framework TEST-HARNESS.md")
    tls = parser.add_mutually_exclusive_group()
    tls.add_argument("--cafile", type=Path)
    tls.add_argument("--insecure", action="store_true", help="accept the bench's self-signed TLS certificate")
    parser.add_argument("--timeout", type=float, default=180,
                        help="scan, membership and activity operation timeout in seconds (default: 180)")
    parser.add_argument("--cec-timeout", type=float, default=5,
                        help="fresh CEC power/routing assertion wait in seconds (default: 5); independent of --timeout")
    parser.add_argument("--firmware", type=Path, metavar="PATH",
                        help="upgrade the Amity system from this update file before running selected tests")
    parser.add_argument("--firmware-timeout", type=float, default=2700,
                        help="firmware upgrade timeout in seconds (default: 2700); independent of --timeout and --cec-timeout")
    parser.add_argument("--harness-host", help="run CEC and reset GPIO I/O on this Pi over SSH")
    parser.add_argument("--harness-root", default="/home/pi/amity/esp32/cec-harness")
    parser.add_argument("--ssh-port", type=int)
    parser.add_argument("--ble-python", default="python3",
                        help="Python executable with dbus-next for the BLE keyboard fixture")
    parser.add_argument("--enrollment-credentials", type=Path,
                        help="private Wi-Fi/Matter JSON; defaults to the Pi's saved enrollment settings")
    parser.add_argument("--enrollment-timeout", type=float, default=240,
                        help="main enrollment deadline in seconds (default: 240)")
    parser.add_argument("--matter-python", default="python3",
                        help="Pi Python executable for the Matter controller")
    parser.add_argument("--no-status-led", action="store_true",
                        help="leave the harness Pi's status LED unchanged")
    parser.add_argument("--output", type=Path, help="private JSON evidence under test-results/")
    parser.add_argument("--log-level", choices=("INFO", "DEBUG"), default="INFO",
                        help="stderr detail: INFO progress (default), DEBUG also HTTP and CEC traffic")
    args = parser.parse_args(argv)
    for option, value in (("--timeout", args.timeout), ("--cec-timeout", args.cec_timeout),
                          ("--firmware-timeout", args.firmware_timeout),
                          ("--enrollment-timeout", args.enrollment_timeout)):
        if not math.isfinite(value) or value <= 0:
            parser.error(f"{option} must be positive and finite")
    if args.test == "firmware_upgrade" and args.firmware is None:
        parser.error("--test firmware_upgrade requires --firmware PATH")
    if args.test == "enrollment" and args.firmware is not None:
        parser.error("--test enrollment cannot be combined with --firmware")
    if args.firmware is not None:
        if firmware_upgrade is None:
            parser.error("--firmware requires a registered firmware upgrade scenario")
        if not args.firmware.is_file():
            parser.error("--firmware must name an existing firmware update file")
    credentials = _named_options(parser, args.dut_credentials, "PATH", Path)
    urls = _named_options(parser, args.dut_url, "URL")
    if args.credentials is None and not credentials:
        parser.error("Provide --credentials or --dut-credentials")
    session_factory = CecSession
    gpio_factory = None
    from .ble_keyboard import LocalKeyboard, SSHKeyboard
    keyboard_factory = partial(LocalKeyboard, python=args.ble_python)
    if args.harness_host:
        from .remote import SSHCecBackend
        from .gpio import SSHPinctrlGPIO
        backend = partial(SSHCecBackend, host=args.harness_host,
                          root=args.harness_root, port=args.ssh_port)
        session_factory = partial(CecSession, backend_factory=backend)
        gpio_factory = partial(SSHPinctrlGPIO, host=args.harness_host,
                               root=args.harness_root, port=args.ssh_port)
        keyboard_factory = partial(SSHKeyboard, host=args.harness_host,
                                   root=args.harness_root, port=args.ssh_port, python=args.ble_python)
    from .enrollment import MatterEnrollment
    enrollment_factory = partial(MatterEnrollment, credentials=args.enrollment_credentials,
                                 host=args.harness_host, root=args.harness_root, port=args.ssh_port,
                                 timeout=args.enrollment_timeout, python=args.matter_python)
    config = RunConfig(credentials=credentials, default_credentials=args.credentials,
                       urls=urls, default_url=args.url, wiring_path=args.wiring,
                       cafile=args.cafile, insecure=args.insecure,
                       timeout=args.timeout, cec_timeout=args.cec_timeout,
                       firmware=args.firmware, firmware_timeout=args.firmware_timeout,
                       session_factory=session_factory, gpio_factory=gpio_factory,
                       keyboard_factory=keyboard_factory, enrollment_factory=enrollment_factory,
                       enrollment_timeout=args.enrollment_timeout)
    selected = (tests if args.test == "all" else
                {} if args.test in ("firmware_upgrade", "enrollment") else {args.test: tests[args.test]})
    if args.firmware is not None:
        selected = {"firmware_upgrade": (firmware_upgrade,), **selected}
    selected_scenarios = [scenario for group in selected.values() for scenario in group]
    required = set()
    if prepare is not None:
        selected_scenarios.insert(0, prepare)
        required.add(0)
    if args.firmware is not None:
        required.add(1 if prepare is not None else 0)
    with stdio_logging(args.log_level):
        started = time.monotonic()
        event("run.start", groups=list(selected),
              tests=[scenario.__name__ for scenario in selected_scenarios],
              log_level=args.log_level, operation_timeout_seconds=config.timeout,
              cec_timeout_seconds=config.cec_timeout, firmware_timeout_seconds=config.firmware_timeout)
        try:
            with (nullcontext() if args.no_status_led else
                  run_status_led(host=args.harness_host, root=args.harness_root, port=args.ssh_port)):
                reports = []
                for index, scenario in enumerate(selected_scenarios):
                    result = run_test(scenario, config)
                    reports.append(result)
                    if result["passed"]:
                        print(f"PASS: {result['test']}", flush=True)
                    else:
                        # Exception text may contain server-echoed secrets or
                        # private URLs. Detailed errors remain in the private
                        # JSON report; stdio carries the type and source stack.
                        print(f"FAIL: {result.get('error_type', 'TestFailure')}: {result['test']}",
                              file=sys.stderr, flush=True)
                        if index in required:
                            break
                if finish is not None and all(result["passed"] for result in reports):
                    result = run_test(finish, config)
                    reports.append(result)
                    if result["passed"]:
                        print(f"PASS: {result['test']}", flush=True)
                    else:
                        print(f"FAIL: {result.get('error_type', 'TestFailure')}: {result['test']}",
                              file=sys.stderr, flush=True)
                report = (reports[0] if len(reports) == 1 else
                          {"passed": all(result["passed"] for result in reports), "tests": reports})
                if args.output:
                    _write_report(args.output, report)
            event("run.finish", passed=report["passed"], tests=len(reports),
                  duration_ms=round((time.monotonic() - started) * 1000, 3))
            if args.output:
                # Last line on both successful and failed runs, after the
                # private report has actually been written.
                event("run.report", path=str(args.output.resolve()))
            return 0 if report["passed"] else 1
        except BaseException as exc:
            event("run.aborted", level=logging.ERROR, **failure_fields(exc),
                  duration_ms=round((time.monotonic() - started) * 1000, 3))
            raise
