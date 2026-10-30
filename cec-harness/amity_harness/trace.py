"""Framework tracing on stderr, independent of functional scenario bodies.

Call sites select diagnostic fields explicitly. Never pass HTTP bodies, headers,
credentials, opaque tokens or arbitrary exception text to this module.
"""

from contextlib import contextmanager
from contextvars import ContextVar
from datetime import datetime, timezone
import json
import logging
import sys
import time
import traceback
from uuid import uuid4


_logger = logging.getLogger("amity_harness")
_logger.addHandler(logging.NullHandler())
_test = ContextVar("amity_harness_trace_test", default="-")
_assertion_marker = object()
_owner_messages = {
    "unknown_component": "The scan owner assertion includes an unknown component",
    "coordinator_identity": "The scan coordinator changed boot or system identity",
    "coordinator_boot_proof": "The captured coordinator boot does not match the scan",
    "bed_scope": "Distributed scan owners need the same active TestBed and CEC session",
    "wire_scope": "A scan GPIO cannot belong to different DUT owners",
    "coordinator_membership": "The scan has no valid ready coordinator membership snapshot",
    "member_proof": "The scan member is unproven, unready, retired or changed boot",
    "member_status": "The scan member is unproven, unready, retired or changed boot",
    "component_owner": "The scan component has a different owner DUT object",
    "member_identity": "The scan member proof has a mismatched DUT identity",
}


def owner_assertion(reason):
    """Mark only a framework-owned literal as safe to include with its cause."""
    error = AssertionError(_owner_messages[reason])
    error._amity_owner_assertion = (_assertion_marker, reason)
    return error


def event(name, *, level=logging.INFO, **fields):
    """Emit one single-line event with selected, JSON-safe diagnostic fields."""
    if _logger.isEnabledFor(level):
        _logger.log(level, "%s %s", name,
                    json.dumps(fields, ensure_ascii=True, sort_keys=True, separators=(",", ":")),
                    extra={"monotonic_ns": time.monotonic_ns()})


def failure_fields(error):
    """Keep bounded cause locations and marked literals; never stringify errors."""
    def details(current):
        fields = {"error_type": type(current).__name__,
                  "stack": [{"file": frame.filename, "line": frame.lineno, "function": frame.name}
                            for frame in traceback.extract_tb(current.__traceback__)[-32:]]}
        marker = getattr(current, "_amity_owner_assertion", None)
        if (type(current) is AssertionError and isinstance(marker, tuple) and len(marker) == 2
                and marker[0] is _assertion_marker and isinstance(marker[1], str) and marker[1] in _owner_messages):
            fields.update(reason=marker[1], message=_owner_messages[marker[1]])
        return fields

    result, causes, seen = details(error), [], {id(error)}
    current = error
    for _ in range(8):
        current = (current.__cause__ if current.__cause__ is not None else
                   None if current.__suppress_context__ else current.__context__)
        if current is None or id(current) in seen:
            break
        seen.add(id(current))
        causes.append(details(current))
    if causes:
        result["causes"] = causes
    return result


class _TraceContext(logging.Filter):
    def __init__(self):
        super().__init__()
        self.run_id = uuid4().hex[:12]
        self.started_ns = time.monotonic_ns()

    def filter(self, record):
        record.run_id = self.run_id
        record.test = _test.get()
        record.elapsed_seconds = (getattr(record, "monotonic_ns", time.monotonic_ns())
                                  - self.started_ns) / 1_000_000_000
        return True


class _Formatter(logging.Formatter):
    def formatTime(self, record, datefmt=None):
        return datetime.fromtimestamp(record.created, timezone.utc).isoformat(
            timespec="milliseconds").replace("+00:00", "Z")


@contextmanager
def stdio_logging(level="INFO"):
    """Install a scoped stderr handler; leave application/root logging alone.

    StreamHandler flushes each event, including when stderr is redirected to a
    file. Restore the prior package configuration even after interruption.
    """
    context = _TraceContext()
    handler = logging.StreamHandler(sys.stderr)
    handler.addFilter(context)
    handler.setFormatter(_Formatter(
        "%(asctime)s +%(elapsed_seconds)09.3fs %(levelname)-7s "
        "run=%(run_id)s test=%(test)s %(message)s"))
    previous = (_logger.handlers[:], _logger.level, _logger.propagate, _logger.disabled)
    _logger.handlers = [handler]
    _logger.setLevel(level)
    _logger.propagate = False
    _logger.disabled = False
    token = _test.set("-")
    try:
        yield context
    finally:
        handler.flush()
        _test.reset(token)
        _logger.handlers, old_level, _logger.propagate, _logger.disabled = previous
        _logger.setLevel(old_level)
        handler.close()


@contextmanager
def test_logging(name):
    """Label all events from the current scenario, including its workers."""
    token = _test.set(name)
    try:
        yield
    finally:
        _test.reset(token)
