"""Stdio traces remain useful during failures and across scenario workers."""

from contextlib import redirect_stderr, redirect_stdout
from contextvars import copy_context
import io
import json
import logging
from pathlib import Path
import tempfile
import threading
import unittest

from amity_harness.config import RunConfig
from amity_harness.runner import main, run_test
from amity_harness.trace import event, failure_fields, owner_assertion, stdio_logging, test_logging


class TraceTests(unittest.TestCase):
    def test_cli_streams_steps_and_results_before_next_test_and_reports_json_last(self):
        output, errors = io.StringIO(), io.StringIO()

        def first():
            self.assertIn("test=first test.start", errors.getvalue())
            event("fixture.ready", dut="amity-dut")
            worker = threading.Thread(target=copy_context().run,
                                      args=(lambda: event("wire.observed", level=logging.DEBUG, gpio=7),))
            worker.start()
            worker.join()
            self.assertIn("test=first wire.observed", errors.getvalue())

        def second():
            self.assertIn("PASS: first", output.getvalue())
            self.assertIn("test=first test.finish", errors.getvalue())
            self.assertIn("test=second test.start", errors.getvalue())
            raise AssertionError("expected physical command was absent")

        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "trace.json"
            with redirect_stdout(output), redirect_stderr(errors):
                code = main({"both": [first, second]},
                            ["--credentials", "/private/credentials.json", "--log-level", "DEBUG",
                             "--output", str(report)])
            self.assertEqual(code, 1)
            self.assertEqual([test["passed"] for test in json.loads(report.read_text())["tests"]], [True, False])
            lines = errors.getvalue().splitlines()
            last = lines[-1].split("run.report ", 1)[1]
            self.assertEqual(json.loads(last), {"path": str(report.resolve())})
            self.assertEqual([path.name for path in Path(directory).iterdir()], ["trace.json"])
        self.assertRegex(lines[0], r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z \+\d+\.\d{3}s INFO")
        self.assertIn("test=second test.failed", errors.getvalue())
        self.assertIn('"function":"second"', errors.getvalue())
        self.assertNotIn("/private/credentials.json", errors.getvalue())

    def test_successful_run_also_ends_with_existing_json_location(self):
        def scenario():
            event("fixture.complete")

        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "result.json"
            errors = io.StringIO()
            with redirect_stdout(io.StringIO()), redirect_stderr(errors):
                self.assertEqual(main(scenario, ["--credentials", "/private/main.json", "--output", str(report)]), 0)
            self.assertTrue(json.loads(report.read_text())["passed"])
            self.assertIn("run.report", errors.getvalue().splitlines()[-1])
            self.assertIn(str(report.resolve()), errors.getvalue().splitlines()[-1])

    def test_default_detail_filters_frames_and_scope_restores_logging_after_interrupt(self):
        logger = logging.getLogger("amity_harness")
        previous = (logger.handlers[:], logger.level, logger.propagate, logger.disabled)
        errors = io.StringIO()
        with redirect_stderr(errors), self.assertRaises(KeyboardInterrupt):
            with stdio_logging():
                event("fixture.ready")
                event("cec.frame", level=logging.DEBUG, frame="04:83")
                raise KeyboardInterrupt()
        self.assertIn("fixture.ready", errors.getvalue())
        self.assertNotIn("cec.frame", errors.getvalue())
        self.assertEqual((logger.handlers, logger.level, logger.propagate, logger.disabled), previous)

    def test_interruption_keeps_last_step_and_safe_failure_locations(self):
        def scenario():
            event("fixture.wait", dut="amity-dut")
            raise KeyboardInterrupt("private-exception-text")

        errors = io.StringIO()
        with redirect_stderr(errors), stdio_logging(), self.assertRaises(KeyboardInterrupt):
            run_test(scenario, RunConfig())
        trace = errors.getvalue()
        self.assertIn("fixture.wait", trace)
        self.assertIn("test.interrupted", trace)
        self.assertIn('"error_type":"KeyboardInterrupt"', trace)
        self.assertIn('"function":"scenario"', trace)
        self.assertNotIn("private-exception-text", trace)

    def test_failed_report_write_does_not_claim_a_report_was_written(self):
        def scenario():
            pass

        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory)  # Writing a file over a directory fails.
            errors = io.StringIO()
            with redirect_stdout(io.StringIO()), redirect_stderr(errors), self.assertRaises(OSError):
                main(scenario, ["--credentials", "/private/main.json", "--output", str(report)])
        self.assertIn("run.aborted", errors.getvalue())
        self.assertNotIn("run.report", errors.getvalue())

    def test_failure_summary_excludes_arbitrary_exception_contents_from_json_and_stdio(self):
        def scenario():
            raise RuntimeError("private-server-token https://private-origin.example")

        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "result.json"
            errors = io.StringIO()
            with redirect_stdout(io.StringIO()), redirect_stderr(errors):
                self.assertEqual(main(scenario, ["--credentials", "/private/main.json", "--output", str(report)]), 1)
            self.assertEqual(json.loads(report.read_text())["error"], "RuntimeError")
            self.assertNotIn("private-server-token", report.read_text())
            self.assertNotIn("private-origin.example", report.read_text())
        self.assertIn("FAIL: RuntimeError: scenario", errors.getvalue())
        self.assertNotIn("private-server-token", errors.getvalue())
        self.assertNotIn("private-origin.example", errors.getvalue())

    def test_delayed_worker_retains_original_test_label(self):
        errors = io.StringIO()
        ready, resume = threading.Event(), threading.Event()

        def worker():
            ready.set()
            resume.wait(2)
            event("worker.delayed")

        with redirect_stderr(errors), stdio_logging():
            with test_logging("original"):
                thread = threading.Thread(target=copy_context().run, args=(worker,))
                thread.start()
                self.assertTrue(ready.wait(2))
            with test_logging("next"):
                resume.set()
                thread.join(2)
                self.assertFalse(thread.is_alive())
                event("next.ready")
        self.assertIn("test=original worker.delayed", errors.getvalue())
        self.assertIn("test=next next.ready", errors.getvalue())

    def test_controlled_assertion_survives_cleanup_cause_without_stringifying_errors(self):
        class PrivateCleanupError(RuntimeError):
            def __str__(self):
                raise AssertionError("Exception text must never be requested")

        def scenario():
            try:
                error = owner_assertion("member_status")
                error.args = ("private-mutated-assertion",)
                raise error
            except AssertionError as primary:
                raise PrivateCleanupError("private-cleanup-token") from primary

        output = io.StringIO()
        with redirect_stderr(output), stdio_logging():
            report = run_test(scenario, RunConfig())
        self.assertEqual(report["error"], "PrivateCleanupError")
        cause = report["failure"]["causes"][0]
        self.assertEqual(cause["error_type"], "AssertionError")
        self.assertEqual(cause["reason"], "member_status")
        self.assertEqual(cause["message"], "The scan member is unproven, unready, retired or changed boot")
        self.assertEqual(cause["stack"][-1]["function"], "scenario")
        serialized = json.dumps(report) + output.getvalue()
        self.assertNotIn("private-", serialized)
        self.assertIn('"causes"', output.getvalue())

    def test_assertion_literal_needs_factory_provenance_and_chain_is_bounded(self):
        literal = "The scan member is unproven, unready, retired or changed boot"
        for error in (AssertionError(literal), AssertionError("private-token"), RuntimeError("private-response")):
            error._amity_owner_assertion = (object(), "member_status")
            error.reason = "member_status"
            error.message = "private-forged-message"
            self.assertNotIn("message", failure_fields(error))
            self.assertNotIn("reason", failure_fields(error))
        error._amity_owner_assertion = (object(), [])
        self.assertEqual(failure_fields(error)["error_type"], "RuntimeError")
        root = owner_assertion("coordinator_identity")
        for _ in range(20):
            outer = RuntimeError("private-chain-token")
            outer.__cause__ = root
            root = outer
        self.assertEqual(len(failure_fields(root)["causes"]), 8)
        cycle = RuntimeError("private-cycle-token")
        cycle.__cause__ = cycle
        self.assertNotIn("causes", failure_fields(cycle))
        hidden = RuntimeError("private-outer")
        hidden.__context__ = owner_assertion("member_status")
        hidden.__suppress_context__ = True
        self.assertNotIn("causes", failure_fields(hidden))


if __name__ == "__main__":
    unittest.main()
