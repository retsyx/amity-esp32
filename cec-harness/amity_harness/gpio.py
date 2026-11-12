"""Bounded physical reset-button holds on the harness Pi."""

from contextlib import contextmanager
import fcntl
import math
import os
from pathlib import Path
import signal
import tempfile
import threading
import time


def _pin(gpio):
    if type(gpio) is not int or not 0 <= gpio <= 27:
        raise ValueError("GPIO must be an integer on the Pi's GPIO header")
    return gpio


def _duration(seconds):
    if (isinstance(seconds, bool) or not isinstance(seconds, (int, float))
            or not math.isfinite(seconds) or not 0 < seconds <= 60):
        raise ValueError("GPIO hold must be positive, finite and at most 60 seconds")
    if 8 < seconds < 10:
        raise ValueError("Add holds must be at most 8 seconds; factory reset requires an explicit 10-second hold")
    return float(seconds)


# A bounded release takes at most one second (the pinctrl timeout). Measured
# from the low edge, this keeps an Add hold under the ten-second reset gesture.
_ADD_SAFETY_SECONDS = 8.5


def _safety_seconds(seconds):
    # Add is bounded independently of the caller. Explicit factory-reset
    # holds retain their requested duration and a small bound.
    return min(seconds + 0.5, _ADD_SAFETY_SECONDS) if seconds < 10 else seconds + 2


@contextmanager
def _signals():
    if threading.current_thread() is not threading.main_thread():
        raise RuntimeError("Physical GPIO holds need a main-thread worker for signal cleanup")
    if any(signal.getitimer(signal.ITIMER_REAL)):
        raise RuntimeError("An existing process timer prevents a bounded GPIO hold")
    previous = {}

    def stop(signum, frame):
        if signum == signal.SIGALRM:
            raise TimeoutError("Physical GPIO hold exceeded its safety deadline")
        raise InterruptedError("Physical GPIO hold interrupted by " + signal.Signals(signum).name)

    try:
        for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP, signal.SIGALRM):
            previous[signum] = signal.signal(signum, stop)
        yield
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        for signum, handler in previous.items():
            signal.signal(signum, handler)


def hold_low(backend, gpio, seconds):
    """Drive low, time the hold in this process, and always release the input.

    Add-range operations have an 8.5-second safety deadline from the low
    edge, so a bounded release still ends the press before ten seconds.
    Durations of ten seconds or longer explicitly retain the existing
    factory-reset gesture. GPIO subprocess operations must themselves be
    bounded (PinctrlGPIO is).
    """
    gpio, seconds = _pin(gpio), _duration(seconds)
    lock_path = Path(tempfile.gettempdir()) / f"amity-reset-gpio-{gpio}.lock"
    fd = os.open(lock_path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        with _signals():
            try:
                backend.low(gpio)
                # Time the safety deadline from the low edge, not from before
                # the bounded pinctrl calls that create it.
                signal.setitimer(signal.ITIMER_REAL, _safety_seconds(seconds))
                time.sleep(seconds)
            finally:
                previous_mask = signal.pthread_sigmask(
                    signal.SIG_BLOCK, {signal.SIGINT, signal.SIGTERM, signal.SIGHUP, signal.SIGALRM})
                try:
                    signal.setitimer(signal.ITIMER_REAL, 0)
                    backend.release(gpio)
                finally:
                    # Pending interruption is delivered only after the bounded
                    # release has finished; keep the caller's original mask.
                    signal.pthread_sigmask(signal.SIG_SETMASK, previous_mask)
    finally:
        os.close(fd)
    return {"gpio": gpio, "seconds": seconds, "released": True}
