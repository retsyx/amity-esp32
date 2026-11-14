"""Shared functional-test CLI, runtime configuration and private reporting."""

import argparse
from contextlib import nullcontext
from dataclasses import replace
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
    dut = DUT(config.main_dut)
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
    dut = DUT(config.main_dut)
    deadline = time.monotonic() + config.enrollment_timeout
    try:
        dut.authenticate()
        system = dut.get_system()
        if system.get("octopus_id") != dut.identity.device_id:
            raise AssertionError(f"DUT HTTP identity does not match {dut.name} in TEST-HARNESS.md")
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


def main(test, argv=None, *, firmware_upgrade=None, prepare=None, finish=None, opt_in_tests=None,
         main_duts=None, firmware_variants=None, scenario_mains=None):
    """Run scenarios; opt-in groups require explicit --test selection, even with all."""
    if firmware_upgrade is not None and not callable(firmware_upgrade):
        raise ValueError("firmware_upgrade must be callable or None")
    if prepare is not None and not callable(prepare):
        raise ValueError("prepare must be callable or None")
    if finish is not None and (not callable(finish) or prepare is None):
        raise ValueError("finish requires a prepare callback and must be callable")
    tests = {test.__name__: test} if callable(test) else dict(test)
    if not tests or any(not isinstance(name, str) for name in tests):
        raise ValueError("Provide named functional tests or nonempty groups of callable tests")
    opt_in_tests = {} if opt_in_tests is None else dict(opt_in_tests)
    if tests.keys() & opt_in_tests.keys():
        raise ValueError("Opt-in test names must be distinct from ordinary test names")
    registered = {**tests, **opt_in_tests}
    if any(not isinstance(name, str) or not name for name in registered):
        raise ValueError("Provide named functional tests or nonempty groups of callable tests")
    for name, group in registered.items():
        if callable(group):
            registered[name] = (group,)
        elif isinstance(group, (tuple, list)) and group and all(callable(scenario) for scenario in group):
            registered[name] = tuple(group)
        else:
            raise ValueError("Provide named functional tests or nonempty groups of callable tests")
    tests = {name: registered[name] for name in tests}
    main_duts = {} if main_duts is None else dict(main_duts)
    firmware_variants = {} if firmware_variants is None else dict(firmware_variants)
    if any(not isinstance(name, str) or not name or not callable(scenario)
           for name, scenario in firmware_variants.items()):
        raise ValueError("Provide named callable firmware variants")
    if firmware_variants.keys() & (registered.keys() | {"all", "enrollment", "firmware_upgrade"}):
        raise ValueError("Firmware variant names must be distinct from other test names")
    firmware_tests = ({"firmware_upgrade": firmware_upgrade} if firmware_upgrade is not None else {})
    firmware_tests.update(firmware_variants)
    scenario_mains = {} if scenario_mains is None else dict(scenario_mains)
    known_scenarios = {scenario for group in registered.values() for scenario in group} | set(firmware_tests.values())
    if (any(scenario not in known_scenarios for scenario in scenario_mains)
            or any(not isinstance(owner, str) or not owner for owner in scenario_mains.values())):
        raise ValueError("Scenario main mappings require registered scenarios and nonempty bench names")
    if (any(name not in registered and name not in firmware_tests for name in main_duts)
            or any(not isinstance(owner, str) or not owner for owner in main_duts.values())):
        raise ValueError("Main DUT mappings require registered test names and nonempty bench names")
    if any(main_duts.get(name, "amity-dut") != "amity-dut"
           or any(scenario_mains.get(scenario, "amity-dut") != "amity-dut" for scenario in group)
           for name, group in tests.items()):
        raise ValueError("Alternate-main scenarios must be registered as opt-in tests")
    firmware_by_main = {}
    for name, scenario in firmware_tests.items():
        owner = main_duts.get(name, "amity-dut")
        if scenario_mains.get(scenario, owner) != owner:
            raise ValueError("A firmware scenario must use its registered main DUT")
        if owner in firmware_by_main:
            raise ValueError("Register only one firmware upgrade scenario per main DUT")
        firmware_by_main[owner] = (name, scenario)
    parser = argparse.ArgumentParser(description="Run Amity functional tests using the CEC harness")
    if "firmware_upgrade" in registered:
        raise ValueError("Register firmware_upgrade using the dedicated callback parameter")
    if "enrollment" in registered:
        raise ValueError("Register enrollment using the prepare callback parameter")
    if "all" in registered:
        raise ValueError("The all test name is reserved for the ordinary test suite")
    choices = [*registered, *firmware_tests,
               *(('enrollment',) if prepare is not None else ()), "all"]
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--test", choices=choices, default=next(iter(tests)),
                           help="scenario to run; defaults to the first ordinary test; all excludes opt-in tests")
    if "soak" in opt_in_tests:
        selection.add_argument("--soak", dest="test", action="store_const", const="soak",
                               help="run the explicitly selected soak scenarios")
        soak_mains = tuple(dict.fromkeys(scenario_mains.get(scenario, main_duts.get("soak", "amity-dut"))
                                         for scenario in registered["soak"]))
        parser.add_argument("--soak-first", choices=soak_mains,
                            help="main DUT for the first soak phase; defaults to the registered order")
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
    parser.add_argument("--cec-timeout", type=float, default=0.5,
                        help="fresh CEC power/routing assertion wait in seconds (default: 0.5); independent of --timeout")
    parser.add_argument("--firmware", type=Path, metavar="PATH",
                        help="upgrade the Amity system from this update file before running selected tests")
    parser.add_argument("--firmware-timeout", type=float, default=2700,
                        help="firmware upgrade timeout in seconds (default: 2700); independent of --timeout and --cec-timeout")
    parser.add_argument("--enrollment-credentials", type=Path,
                        help="private Wi-Fi/Matter JSON; defaults to the Pi's saved enrollment settings")
    parser.add_argument("--dut-enrollment-credentials", action="append", default=[], metavar="DUT=PATH",
                        help="per-DUT private Wi-Fi/Matter JSON; repeat for multiple main DUTs")
    parser.add_argument("--enrollment-timeout", type=float, default=240,
                        help="main enrollment deadline in seconds (default: 240)")
    parser.add_argument("--no-status-led", action="store_true",
                        help="leave the harness Pi's status LED unchanged")
    parser.add_argument("--output", type=Path, help="private JSON evidence under test-results/")
    parser.add_argument("--log-level", choices=("INFO", "DEBUG"), default="INFO",
                        help="stderr detail: INFO progress (default), DEBUG also HTTP and CEC traffic")
    args = parser.parse_args(argv)
    soak_first = getattr(args, "soak_first", None)
    if soak_first is not None and args.test != "soak":
        parser.error("--soak-first requires --soak or --test soak")
    for option, value in (("--timeout", args.timeout), ("--cec-timeout", args.cec_timeout),
                          ("--firmware-timeout", args.firmware_timeout),
                          ("--enrollment-timeout", args.enrollment_timeout)):
        if not math.isfinite(value) or value <= 0:
            parser.error(f"{option} must be positive and finite")
    group_main = main_duts.get(args.test, "amity-dut")
    selected = (tests if args.test == "all" else
                {} if args.test in firmware_tests or args.test == "enrollment" else {args.test: registered[args.test]})
    ordinary_plan = [(scenario, scenario_mains.get(scenario, group_main), False)
                     for group in selected.values() for scenario in group]
    if soak_first is not None:
        # Rotate at the requested main's first block, retaining each phase's
        # scenarios and owner mappings. Preparation/reset still own transitions.
        first = next(index for index, (_, owner, _) in enumerate(ordinary_plan) if owner == soak_first)
        ordinary_plan = ordinary_plan[first:] + ordinary_plan[:first]
    selected_main = ordinary_plan[0][1] if ordinary_plan else group_main
    if any(owner != selected_main for _, owner, _ in ordinary_plan) and (prepare is None or finish is None):
        parser.error("Changing the main DUT requires preparation and reset callbacks")
    if args.test in firmware_tests and args.firmware is None:
        parser.error(f"--test {args.test} requires --firmware PATH")
    if args.test == "enrollment" and args.firmware is not None:
        parser.error("--test enrollment cannot be combined with --firmware")
    if args.firmware is not None:
        if selected_main not in firmware_by_main:
            parser.error("--firmware requires an upgrade scenario for the selected main DUT")
        if not args.firmware.is_file():
            parser.error("--firmware must name an existing firmware update file")
    credentials = _named_options(parser, args.dut_credentials, "PATH", Path)
    enrollment_credentials = _named_options(parser, args.dut_enrollment_credentials, "PATH", Path)
    urls = _named_options(parser, args.dut_url, "URL")
    if args.credentials is None and not credentials:
        parser.error("Provide --credentials or --dut-credentials")
    from .ble_keyboard import LocalKeyboard
    keyboard_factory = partial(LocalKeyboard, python=sys.executable)
    from .enrollment import MatterEnrollment
    def enrollment_factory_for(owner):
        return partial(MatterEnrollment, credentials=enrollment_credentials.get(owner, args.enrollment_credentials),
                       timeout=args.enrollment_timeout, python=sys.executable)

    config = RunConfig(main_dut=selected_main, credentials=credentials, default_credentials=args.credentials,
                       urls=urls, default_url=args.url, wiring_path=args.wiring,
                       cafile=args.cafile, insecure=args.insecure,
                       timeout=args.timeout, cec_timeout=args.cec_timeout,
                       firmware=args.firmware, firmware_timeout=args.firmware_timeout,
                       session_factory=CecSession,
                       keyboard_factory=keyboard_factory, enrollment_factory=enrollment_factory_for(selected_main),
                       enrollment_timeout=args.enrollment_timeout)
    plan = ordinary_plan
    if args.firmware is not None:
        upgrade_name, upgrade = firmware_by_main[selected_main]
        selected = {upgrade_name: (upgrade,), **selected}
        plan.insert(0, (upgrade, selected_main, True))
    if prepare is not None:
        plan.insert(0, (prepare, selected_main, True))
    with stdio_logging(args.log_level):
        started = time.monotonic()
        event("run.start", groups=list(selected),
              tests=[scenario.__name__ for scenario, _, _ in plan],
              log_level=args.log_level, operation_timeout_seconds=config.timeout,
              cec_timeout_seconds=config.cec_timeout, firmware_timeout_seconds=config.firmware_timeout)
        try:
            with (nullcontext() if args.no_status_led else
                  run_status_led()):
                reports = []

                def execute(scenario, active_config):
                    result = run_test(scenario, active_config)
                    reports.append(result)
                    if result["passed"]:
                        print(f"PASS: {result['test']}", flush=True)
                    else:
                        # Exception text may contain server-echoed secrets or
                        # private URLs. Detailed errors remain in the private
                        # JSON report; stdio carries the type and source stack.
                        print(f"FAIL: {result.get('error_type', 'TestFailure')}: {result['test']}",
                              file=sys.stderr, flush=True)
                    return result

                for scenario, owner, required in plan:
                    if owner != config.main_dut:
                        # A failed scenario keeps its main available for
                        # diagnosis; switching roles must never erase it.
                        if not all(result["passed"] for result in reports):
                            break
                        if not execute(finish, config)["passed"]:
                            break
                        previous = config
                        config = replace(previous, main_dut=owner, enrollment_factory=enrollment_factory_for(owner))
                        config.admin_origins.update(previous.admin_origins)
                        if not execute(prepare, config)["passed"]:
                            break
                    result = execute(scenario, config)
                    # Retained state or failed cleanup leaves a board dirty;
                    # later scenarios would fail for unrelated reasons.
                    if not result["passed"] and (required or any(
                            bed.get("dut_state_retained") is True or bed.get("cleanup_errors")
                            for bed in result.get("testbeds", []))):
                        break
                if finish is not None and all(result["passed"] for result in reports):
                    execute(finish, config)
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
