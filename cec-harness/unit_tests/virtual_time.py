"""Deterministic virtual time for the harness unit tests.

``VirtualClock`` replaces the module-level ``time``, ``threading`` and
``queue`` names, and ``ThreadPoolExecutor``, inside the harness, functional
test and unit test modules. Nothing else in the process sees it: the real
``time`` module, the standard library and the test runner keep real time.

Clock reads return virtual time, and each read moves it forward by one
nanosecond so successive reads differ as they do on a real clock. Sleeps,
timed condition and event waits, lock timeouts, joins, queue timeouts and
timers wait for virtual deadlines.
Virtual time moves only when every managed thread is blocked and none of them
can proceed; it then jumps straight to the earliest deadline. A slow runner
therefore cannot expire a deadline early, and no test waits in real time.

Threads created through the patched ``threading`` name are managed. A virtual
wait from any other thread raises ``VirtualClockError``. Code that blocks in a
real call (a socket, a subprocess, an unpatched primitive) stays runnable, so
time stops while it runs; if it never returns the waiters fail with
``VirtualClockStall`` instead of hanging. A thread that reads the clock in a
loop without ever waiting also fails with ``VirtualClockStall``.

Typical use::

    def setUp(self):
        self.clock = use_virtual_time(self)
"""
import collections
import concurrent.futures
import importlib
import pkgutil
import queue as _queue
import sys
import threading as _threading
import time as _time
import traceback
from pathlib import Path
from unittest import mock

__all__ = ["VirtualClock", "VirtualClockError", "VirtualClockStall", "VirtualDeadlock", "use_virtual_time"]

_THIS_FILE = Path(__file__).resolve()
_UNIT_TESTS = _THIS_FILE.parent
_PACKAGES = ("amity_harness", "functional_tests")
_swept = False


class VirtualClockError(RuntimeError):
    """The virtual clock was used in a way it cannot simulate."""


class VirtualDeadlock(VirtualClockError):
    """Every managed thread waits forever: nothing can ever wake them."""


class VirtualClockStall(VirtualClockError):
    """A managed thread made no progress for a long real-time interval."""


class _Managed:
    __slots__ = ("thread", "predicate", "deadline_ns", "spins")

    def __init__(self, thread):
        self.thread = thread
        self.predicate = None
        self.deadline_ns = None
        self.spins = 0


class _Proxy:
    """A module stand-in: overridden names first, the real module otherwise."""

    def __init__(self, real, **overrides):
        self.__dict__.update(overrides)
        self._real = real

    def __getattr__(self, name):
        return getattr(self._real, name)

    def __repr__(self):
        return f"<virtual {self._real.__name__}>"


class VirtualClock:
    # Real seconds a waiter tolerates with no state change anywhere before it
    # reports a stall. Generous: unit tests finish in well under a second.
    stall_seconds = 30.0
    # Real seconds a complete deadlock is given for an unmanaged thread to
    # change something before it is reported.
    deadlock_grace_seconds = 2.0
    # Clock reads one thread may make between waits before it is reported as
    # a busy loop.
    spin_limit = 200_000
    # Virtual nanoseconds each clock read adds, so successive reads differ.
    tick_ns = 1

    def __init__(self, start=1000.0, *, wall_start=1_767_225_600.0):
        self._lock = _threading.RLock()
        self._cv = _threading.Condition(self._lock)
        self._now_ns = round(start * 1e9)
        self._wall_offset_ns = round(wall_start * 1e9) - self._now_ns
        self._threads = {}
        self._running = 0
        self._generation = 0
        self._error = None
        self._closed = False
        self._patches = []
        self._manage(_threading.current_thread())
        self._running += 1
        clock = self

        class Thread(_VThread):
            _vclock = clock

        class Timer(_VTimer):
            _vclock = clock

        self.Thread, self.Timer = Thread, Timer
        self.time = _Proxy(_time, monotonic=self.monotonic, monotonic_ns=self.monotonic_ns,
                           perf_counter=self.monotonic, perf_counter_ns=self.monotonic_ns,
                           time=self.time_seconds, time_ns=self.time_ns, sleep=self.sleep)
        self.threading = _Proxy(_threading, Lock=self.Lock, RLock=self.RLock, Condition=self.Condition,
                                Event=self.Event, Thread=Thread, Timer=Timer)
        self.queue = _Proxy(_queue, Queue=self.Queue)

    # Clock reads -----------------------------------------------------------

    @property
    def now_ns(self):
        """Virtual time for assertions: reading it neither ticks nor counts as a spin."""
        return self._now_ns

    @property
    def now(self):
        return self._now_ns / 1e9

    def monotonic_ns(self):
        with self._lock:
            self._count_spin()
            self._now_ns += self.tick_ns
            return self._now_ns

    def monotonic(self):
        return self.monotonic_ns() / 1e9

    def time_ns(self):
        return self.monotonic_ns() + self._wall_offset_ns

    def time_seconds(self):
        return self.time_ns() / 1e9

    def sleep(self, seconds):
        if seconds < 0:
            raise ValueError("sleep length must be non-negative")
        with self._lock:
            self._wait(lambda: False, self._deadline(seconds))

    def settle(self):
        """Wait, without moving time, until every other managed thread is blocked."""
        me = _threading.get_ident()

        def others_blocked():
            return not self._running and not any(
                self._ready(record) for ident, record in self._threads.items()
                if ident != me and record.predicate is not None)

        with self._lock:
            self._wait(others_blocked, None)

    def advance(self, seconds):
        """Move virtual time forward now, waking every deadline it passes."""
        if seconds < 0:
            raise ValueError("virtual time cannot move backwards")
        with self._lock:
            self._now_ns += round(seconds * 1e9)
            self._notify()

    # Primitive factories --------------------------------------------------

    def Lock(self):
        return _VLock(self)

    def RLock(self):
        return _VRLock(self)

    def Condition(self, lock=None):
        return _VCondition(self, lock)

    def Event(self):
        return _VEvent(self)

    def Queue(self, maxsize=0):
        return _VQueue(self, maxsize)

    def ThreadPoolExecutor(self, max_workers=None, thread_name_prefix="", initializer=None, initargs=()):
        return _VExecutor(self, max_workers, thread_name_prefix, initializer, initargs)

    # Installation ---------------------------------------------------------

    def install(self, *modules):
        """Replace time names in the given modules (default: all harness and unit test modules)."""
        if not modules:
            modules = _default_modules()
        replacements = {"time": (_time, self.time), "threading": (_threading, self.threading),
                        "queue": (_queue, self.queue), "Queue": (_queue.Queue, self.Queue),
                        "ThreadPoolExecutor": (concurrent.futures.ThreadPoolExecutor, self.ThreadPoolExecutor)}
        for module in modules:
            for name, (real, fake) in replacements.items():
                if module.__dict__.get(name) is real:
                    patcher = mock.patch.object(module, name, fake)
                    patcher.start()
                    self._patches.append(patcher)
        return self

    def close(self):
        """Restore the real names. Threads still waiting continue in real time."""
        while self._patches:
            self._patches.pop().stop()
        with self._lock:
            self._closed = True
            self._notify()

    # Scheduler ------------------------------------------------------------

    def _manage(self, thread):
        self._threads[thread.ident] = _Managed(thread)

    def _deadline(self, seconds):
        if seconds is None:
            return None
        return self._now_ns + max(0, round(seconds * 1e9))

    def _notify(self):
        self._generation += 1
        self._cv.notify_all()

    def _count_spin(self):
        record = self._threads.get(_threading.get_ident())
        if record is not None and not self._closed:
            record.spins += 1
            if record.spins > self.spin_limit:
                record.spins = 0
                raise VirtualClockStall(
                    f"{record.thread.name} read the virtual clock {self.spin_limit} times without waiting; "
                    "a loop that never sleeps or waits cannot see virtual time advance")

    def _ready(self, record):
        return record.predicate() or (record.deadline_ns is not None and self._now_ns >= record.deadline_ns)

    def _advance_if_idle(self):
        """With every managed thread blocked, jump to the earliest deadline."""
        if self._running or self._error is not None:
            return False
        blocked = [record for record in self._threads.values() if record.predicate is not None]
        if any(self._ready(record) for record in blocked):
            self._cv.notify_all()
            return False
        deadlines = [record.deadline_ns for record in blocked if record.deadline_ns is not None]
        if not deadlines:
            return None
        self._now_ns = max(self._now_ns, min(deadlines))
        self._notify()
        return True

    def _wait(self, predicate, deadline_ns):
        """Block the calling thread until predicate() or the deadline. Hold self._lock."""
        if predicate():
            return True
        if self._closed:
            return self._wait_real(predicate, deadline_ns)
        record = self._threads.get(_threading.get_ident())
        if record is None:
            raise VirtualClockError(
                f"{_threading.current_thread().name} is not managed by the virtual clock; "
                "create threads through the patched threading module")
        if deadline_ns is not None and self._now_ns >= deadline_ns:
            return False
        record.predicate, record.deadline_ns, record.spins = predicate, deadline_ns, 0
        self._running -= 1
        try:
            while True:
                if self._error is not None:
                    raise self._error
                if predicate():
                    return True
                if deadline_ns is not None and self._now_ns >= deadline_ns:
                    return False
                if self._closed:
                    return self._wait_real(predicate, deadline_ns)
                advanced = self._advance_if_idle()
                if advanced:
                    continue
                generation = self._generation
                if advanced is None:
                    self._cv.wait(self.deadlock_grace_seconds)
                    if generation == self._generation and self._advance_if_idle() is None:
                        self._error = VirtualDeadlock(self._describe("every managed thread waits forever"))
                        self._notify()
                    continue
                if not self._cv.wait(self.stall_seconds) and generation == self._generation:
                    self._error = VirtualClockStall(self._describe(
                        f"no progress for {self.stall_seconds:g} real seconds; a managed thread is "
                        "blocked outside the virtual clock"))
                    self._notify()
        finally:
            record.predicate = record.deadline_ns = None
            record.spins = 0
            self._running += 1

    def _wait_real(self, predicate, deadline_ns):
        end = None if deadline_ns is None else _time.monotonic() + max(0, deadline_ns - self._now_ns) / 1e9
        while not predicate():
            remaining = None if end is None else end - _time.monotonic()
            if remaining is not None and remaining <= 0:
                return False
            self._cv.wait(remaining)
        return True

    def _describe(self, problem):
        frames = sys._current_frames()
        lines = [f"Virtual clock at {self._now_ns / 1e9:.6f} s: {problem}."]
        for ident, record in self._threads.items():
            state = "running" if record.predicate is None else (
                "waiting" if record.deadline_ns is None else f"waiting until {record.deadline_ns / 1e9:.6f} s")
            lines.append(f"--- {record.thread.name} ({state})")
            frame = frames.get(ident)
            if frame is not None:
                lines.append("".join(traceback.format_stack(frame, limit=6)).rstrip())
        return "\n".join(lines)


def _default_modules():
    global _swept
    if not _swept:
        # Import every harness module once so lazily imported ones are patched too.
        _swept = True
        for package in _PACKAGES:
            try:
                root = importlib.import_module(package)
            except ImportError:
                continue
            for info in pkgutil.iter_modules(root.__path__, package + "."):
                try:
                    importlib.import_module(info.name)
                except Exception:
                    pass  # Optional runtime dependencies are absent on some hosts.
    return [module for module in list(sys.modules.values()) if _eligible(module)]


_eligibility = {}


def _eligible(module):
    """Harness, functional test and unit test modules, decided once per module."""
    key = id(module)
    cached = _eligibility.get(key)
    if cached is not None and cached[0] is module:
        return cached[1]
    name = getattr(module, "__name__", None) or ""
    file = getattr(module, "__file__", None)
    path = Path(file).resolve() if file else None
    result = path != _THIS_FILE and (name.split(".")[0] in _PACKAGES
                                     or (path is not None and path.parent == _UNIT_TESTS))
    _eligibility[key] = (module, result)
    return result


def use_virtual_time(test, *modules, start=1000.0):
    """Install a VirtualClock for one test and restore real time at cleanup."""
    clock = VirtualClock(start)
    clock.install(*modules)
    test.addCleanup(clock.close)
    return clock


class _VLock:
    _reentrant = False

    def __init__(self, clock):
        self._clock = clock
        self._owner = None
        self._count = 0

    def _free_for(self, me):
        return lambda: self._owner is None or (self._reentrant and self._owner == me)

    def acquire(self, blocking=True, timeout=-1):
        if not blocking and timeout != -1:
            raise ValueError("can't specify a timeout for a non-blocking call")
        if timeout is not None and timeout < 0 and timeout != -1:
            raise ValueError("timeout value must be a non-negative number")
        clock, me = self._clock, _threading.get_ident()
        with clock._lock:
            free = self._free_for(me)
            if not free():
                if not blocking:
                    return False
                if not clock._wait(free, None if timeout == -1 else clock._deadline(timeout)):
                    return False
            if self._owner == me and self._reentrant:
                self._count += 1
            else:
                self._owner, self._count = me, 1
            return True

    def release(self):
        clock = self._clock
        with clock._lock:
            if self._owner is None or (self._reentrant and self._owner != _threading.get_ident()):
                raise RuntimeError("cannot release un-acquired lock")
            self._count -= 1
            if not self._count:
                self._owner = None
                clock._notify()

    def locked(self):
        return self._owner is not None

    def __enter__(self):
        return self.acquire()

    def __exit__(self, *exc):
        self.release()

    def _is_owned(self):
        return self._owner == _threading.get_ident()

    def _release_save(self):
        with self._clock._lock:
            if not self._is_owned():
                raise RuntimeError("cannot release un-acquired lock")
            count, self._owner, self._count = self._count, None, 0
            self._clock._notify()
            return count

    def _acquire_restore(self, count):
        clock, me = self._clock, _threading.get_ident()
        with clock._lock:
            clock._wait(lambda: self._owner is None, None)
            self._owner, self._count = me, count


class _VRLock(_VLock):
    _reentrant = True


class _VCondition:
    def __init__(self, clock, lock=None):
        self._clock = clock
        self._lock = lock if lock is not None else _VRLock(clock)
        if not isinstance(self._lock, _VLock):
            raise VirtualClockError("a virtual Condition needs a lock from the same virtual threading module")
        self.acquire, self.release = self._lock.acquire, self._lock.release
        self._is_owned = self._lock._is_owned  # As threading.Condition provides.
        self._waiters = collections.deque()

    def __enter__(self):
        return self._lock.__enter__()

    def __exit__(self, *exc):
        return self._lock.__exit__(*exc)

    def wait(self, timeout=None):
        if not self._lock._is_owned():
            raise RuntimeError("cannot wait on un-acquired lock")
        clock, token = self._clock, [False]
        with clock._lock:
            deadline = clock._deadline(timeout)
            self._waiters.append(token)
            saved = self._lock._release_save()
            try:
                clock._wait(lambda: token[0], deadline)
            finally:
                if not token[0]:
                    try:
                        self._waiters.remove(token)
                    except ValueError:
                        pass
                self._lock._acquire_restore(saved)
            return token[0]

    def wait_for(self, predicate, timeout=None):
        clock = self._clock
        end = None if timeout is None else clock.monotonic() + timeout
        result = predicate()
        while not result:
            remaining = None if end is None else end - clock.monotonic()
            if remaining is not None and remaining <= 0:
                break
            self.wait(remaining)
            result = predicate()
        return result

    def notify(self, n=1):
        if not self._lock._is_owned():
            raise RuntimeError("cannot notify on un-acquired lock")
        with self._clock._lock:
            for _ in range(min(n, len(self._waiters))):
                self._waiters.popleft()[0] = True
            self._clock._notify()

    def notify_all(self):
        self.notify(len(self._waiters))

    notifyAll = notify_all


class _VEvent:
    def __init__(self, clock):
        self._clock = clock
        self._flag = False

    def is_set(self):
        return self._flag

    isSet = is_set

    def set(self):
        with self._clock._lock:
            self._flag = True
            self._clock._notify()

    def clear(self):
        with self._clock._lock:
            self._flag = False

    def wait(self, timeout=None):
        clock = self._clock
        with clock._lock:
            return clock._wait(lambda: self._flag, clock._deadline(timeout))


class _VQueue:
    def __init__(self, clock, maxsize=0):
        self._clock = clock
        self.maxsize = maxsize
        self.queue = collections.deque()
        self.unfinished_tasks = 0

    def qsize(self):
        return len(self.queue)

    def empty(self):
        return not self.queue

    def full(self):
        return 0 < self.maxsize <= len(self.queue)

    def put(self, item, block=True, timeout=None):
        clock = self._clock
        with clock._lock:
            if self.full():
                if not block or not clock._wait(lambda: not self.full(), clock._deadline(timeout)):
                    raise _queue.Full
            self.queue.append(item)
            self.unfinished_tasks += 1
            clock._notify()

    def get(self, block=True, timeout=None):
        clock = self._clock
        with clock._lock:
            if not self.queue:
                if not block or not clock._wait(lambda: bool(self.queue), clock._deadline(timeout)):
                    raise _queue.Empty
            item = self.queue.popleft()
            clock._notify()
            return item

    def put_nowait(self, item):
        self.put(item, block=False)

    def get_nowait(self):
        return self.get(block=False)

    def task_done(self):
        with self._clock._lock:
            if self.unfinished_tasks <= 0:
                raise ValueError("task_done() called too many times")
            self.unfinished_tasks -= 1
            self._clock._notify()

    def join(self):
        with self._clock._lock:
            self._clock._wait(lambda: not self.unfinished_tasks, None)


class _VThread(_threading.Thread):
    """A real thread whose blocking is accounted for by its virtual clock."""

    _vclock = None

    def start(self):
        clock = self._vclock
        with clock._lock:
            clock._running += 1  # Counted from now so time cannot move before it runs.
        self._vdone = False
        try:
            super().start()
        except BaseException:
            with clock._lock:
                clock._running -= 1
                clock._notify()
            raise

    def run(self):
        clock = self._vclock
        with clock._lock:
            clock._manage(self)
        try:
            self._vrun()
        finally:
            with clock._lock:
                clock._threads.pop(_threading.get_ident(), None)
                clock._running -= 1
                self._vdone = True
                clock._notify()

    def _vrun(self):
        _threading.Thread.run(self)

    def join(self, timeout=None):
        if self is _threading.current_thread():
            raise RuntimeError("cannot join current thread")
        if not self._started.is_set():
            raise RuntimeError("cannot join thread before it is started")
        clock = self._vclock
        with clock._lock:
            clock._wait(lambda: self._vdone, clock._deadline(timeout))
            done = self._vdone
        if done:
            super().join()


class _VTimer(_VThread):
    def __init__(self, interval, function, args=None, kwargs=None):
        super().__init__()
        self.interval, self.function = interval, function
        self.args, self.kwargs = list(args or ()), dict(kwargs or {})
        self.finished = _VEvent(self._vclock)

    def cancel(self):
        self.finished.set()

    def _vrun(self):
        self.finished.wait(self.interval)
        if not self.finished.is_set():
            self.function(*self.args, **self.kwargs)
        self.finished.set()


class _VFuture(concurrent.futures.Future):
    """A Future whose result() and exception() wait in virtual time."""

    def __init__(self, clock):
        super().__init__()
        self._vclock = clock
        self._vsettled = _VEvent(clock)
        self.add_done_callback(lambda _: self._vsettled.set())

    def result(self, timeout=None):
        if not self._vsettled.wait(timeout):
            raise concurrent.futures.TimeoutError()
        return super().result(0)

    def exception(self, timeout=None):
        if not self._vsettled.wait(timeout):
            raise concurrent.futures.TimeoutError()
        return super().exception(0)


class _VExecutor:
    def __init__(self, clock, max_workers, thread_name_prefix, initializer, initargs):
        self._clock = clock
        self._max_workers = max_workers or 4
        self._prefix = thread_name_prefix or "virtual-executor"
        self._initializer, self._initargs = initializer, initargs
        self._work = _VQueue(clock)
        self._workers = []
        self._shutdown = False

    def submit(self, fn, /, *args, **kwargs):
        if self._shutdown:
            raise RuntimeError("cannot schedule new futures after shutdown")
        future = _VFuture(self._clock)
        self._work.put((future, fn, args, kwargs))
        if len(self._workers) < self._max_workers:
            worker = self._clock.Thread(target=self._worker, daemon=True,
                                        name=f"{self._prefix}_{len(self._workers)}")
            self._workers.append(worker)
            worker.start()
        return future

    def _worker(self):
        if self._initializer is not None:
            self._initializer(*self._initargs)
        while True:
            item = self._work.get()
            if item is None:
                return
            future, fn, args, kwargs = item
            if not future.set_running_or_notify_cancel():
                continue
            try:
                result = fn(*args, **kwargs)
            except BaseException as error:
                future.set_exception(error)
            else:
                future.set_result(result)

    def map(self, fn, *iterables, timeout=None, chunksize=1):
        futures = [self.submit(fn, *args) for args in zip(*iterables)]
        return (future.result(timeout) for future in futures)

    def shutdown(self, wait=True, *, cancel_futures=False):
        if self._shutdown:
            return
        self._shutdown = True
        if cancel_futures:
            with self._clock._lock:
                pending = [item for item in self._work.queue if item is not None]
                self._work.queue.clear()
            for future, *_ in pending:
                future.cancel()
        for _ in self._workers:
            self._work.put(None)
        if wait:
            for worker in self._workers:
                worker.join()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.shutdown(wait=True)
        return False
