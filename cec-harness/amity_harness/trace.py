"""Framework tracing on stderr, independent of functional scenario bodies.

Call sites select diagnostic fields explicitly. Never pass HTTP bodies, headers,
credentials, opaque tokens or exception text to this module.
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


def event(name, *, level=logging.INFO, **fields):
    """Emit one single-line event with selected, JSON-safe diagnostic fields."""
    if _logger.isEnabledFor(level):
        _logger.log(level, "%s %s", name,
                    json.dumps(fields, ensure_ascii=True, sort_keys=True, separators=(",", ":")),
                    extra={"monotonic_ns": time.monotonic_ns()})


def failure_fields(error):
    """Keep failure locations without exception text, source lines or locals."""
    return {"error_type": type(error).__name__,
            "stack": [{"file": frame.filename, "line": frame.lineno, "function": frame.name}
                      for frame in traceback.extract_tb(error.__traceback__)]}


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
