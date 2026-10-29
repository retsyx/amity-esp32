"""Shared functional-test CLI, runtime configuration and private reporting."""

import argparse
from functools import partial
import json
import math
import os
from pathlib import Path
import sys

from .cec import CecSession
from .config import RunConfig, track_testbeds, use_config


def run_test(test, config):
    """Execute a scenario; resource contexts own cleanup before reporting."""
    report = {"test": test.__name__, "passed": False}
    with use_config(config), track_testbeds() as beds:
        try:
            test()
            report["passed"] = True
        except Exception as exc:
            report["error"] = f"{type(exc).__name__}: {exc}"
        finally:
            report["testbeds"] = [bed.report for bed in beds]
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


def main(test, argv=None):
    """Run registered scenarios or groups with the shared functional-test CLI."""
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
    parser.add_argument("--test", choices=[*tests, "all"], default=next(iter(tests)),
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
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--harness-host", help="run CEC and reset GPIO I/O on this Pi over SSH")
    parser.add_argument("--harness-root", default="/home/pi/amity/esp32/cec-harness")
    parser.add_argument("--ssh-port", type=int)
    parser.add_argument("--output", type=Path, help="private JSON evidence under test-results/")
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive and finite")
    credentials = _named_options(parser, args.dut_credentials, "PATH", Path)
    urls = _named_options(parser, args.dut_url, "URL")
    if args.credentials is None and not credentials:
        parser.error("Provide --credentials or --dut-credentials")
    session_factory = CecSession
    gpio_factory = None
    if args.harness_host:
        from .remote import SSHCecBackend
        from .gpio import SSHPinctrlGPIO
        backend = partial(SSHCecBackend, host=args.harness_host,
                          root=args.harness_root, port=args.ssh_port)
        session_factory = partial(CecSession, backend_factory=backend)
        gpio_factory = partial(SSHPinctrlGPIO, host=args.harness_host,
                               root=args.harness_root, port=args.ssh_port)
    config = RunConfig(credentials=credentials, default_credentials=args.credentials,
                       urls=urls, default_url=args.url, wiring_path=args.wiring,
                       cafile=args.cafile, insecure=args.insecure,
                       timeout=args.timeout, session_factory=session_factory, gpio_factory=gpio_factory)
    selected = tests if args.test == "all" else {args.test: tests[args.test]}
    reports = [run_test(scenario, config) for group in selected.values() for scenario in group]
    report = (reports[0] if len(reports) == 1 else
              {"passed": all(result["passed"] for result in reports), "tests": reports})
    if args.output:
        _write_report(args.output, report)
    for result in reports:
        if result["passed"]:
            print(f"PASS: {result['test']}")
        else:
            print("FAIL: " + result.get("error", result["test"]), file=sys.stderr)
    if not report["passed"] and args.output:
        print(f"Evidence: {args.output}", file=sys.stderr)
    return 0 if report["passed"] else 1
