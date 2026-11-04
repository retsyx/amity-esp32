"""Continuous pin collection must survive serialized responder/RPC work."""

import errno
from collections import deque
from queue import Empty, Queue
import threading
import time
import unittest
from unittest.mock import patch

from amity_harness.cec import CecError, _Event
from amity_harness.cec_pin import PinMonitor
from unit_tests.test_cec_pin import edge, waveform


class PinMonitorTests(unittest.TestCase):
    def wait(self, predicate):
        deadline = time.monotonic() + 1
        while not predicate():
            if time.monotonic() >= deadline:
                self.fail("The independent pin reader did not progress")
            time.sleep(.001)

    def test_collection_drains_all_edges_without_a_receive_rpc(self):
        queue = Queue()
        started = time.monotonic_ns() - 200_000_000
        edges, end = waveform(b"\x04\x44\x04", started=started)
        for notice in [*edges, edge(end, False)]:
            event = _Event();event.event = notice.event;event.timestamp_ns = notice.ts
            queue.put(event)
        calls = []
        def ioctl(fd, direction, number, value):
            calls.append((fd, direction, number))
            try:
                return queue.get_nowait()
            except Empty:
                raise OSError(errno.EAGAIN, "drained")
        # select is only the edge-reader wakeup; responder configuration and
        # receive RPCs never participate in this independent drain loop.
        with patch("amity_harness.cec_pin.select.select", side_effect=lambda *args: ([], [], [])), \
             patch("amity_harness.cec_pin._ioctl", side_effect=ioctl):
            monitor = PinMonitor(123)
            try:
                self.wait(lambda: bool(monitor._frames))
                self.assertTrue(queue.empty())
                frame = monitor.receive(.01)
                self.assertEqual(frame.raw, b"\x04\x44\x04")
                self.assertEqual(frame.timestamp_ns, started)
                self.assertEqual(frame.eom, (False, False, True))
                self.assertIsNone(monitor.receive(.001))
            finally:
                monitor.close()
        self.assertTrue(calls)
        self.assertEqual(set(calls), {(123, 3, 7)})

    def test_lost_pin_event_fails_receive_and_close(self):
        notice = _Event();notice.event = 3;notice.flags = 2
        notice.timestamp_ns = time.monotonic_ns()
        with patch("amity_harness.cec_pin.select.select", return_value=([], [], [123])), \
             patch("amity_harness.cec_pin._ioctl", return_value=notice):
            monitor = PinMonitor(123)
            self.wait(lambda: monitor._error is not None)
            with self.assertRaisesRegex(CecError, "lost physical coverage"):
                monitor.receive(.01)
            with self.assertRaisesRegex(CecError, "lost physical coverage"):
                monitor.close()

    def test_empty_response_is_bounded_and_makes_no_receive_coverage_promise(self):
        monitor = object.__new__(PinMonitor)
        monitor._condition = threading.Condition()
        monitor._frames = deque()
        monitor._error = None
        monitor._stop = threading.Event()
        started = time.monotonic()
        self.assertIsNone(monitor.receive(.01))
        self.assertGreaterEqual(time.monotonic() - started, .009)
        self.assertLess(time.monotonic() - started, .2)
        self.assertFalse(hasattr(monitor, "_through_ns"))

    def test_kernel_eagain_before_delayed_timestamped_high_cannot_invent_missing_edge(self):
        queue = Queue()
        started = time.monotonic_ns() - 1_000_000_000
        edges, end = waveform(b"\x04\x44\x04", started=started)
        def add(notices):
            for notice in notices:
                event = _Event();event.event = notice.event;event.timestamp_ns = notice.ts
                queue.put(event)
        def ioctl(*args):
            try:
                return queue.get_nowait()
            except Empty:
                raise OSError(errno.EAGAIN, "upstream worker has not delivered more pin events")
        add(edges[:3])  # A real data LOW; its timely HIGH is not delivered yet.
        with patch("amity_harness.cec_pin.select.select", side_effect=lambda *args: ([], [], [])), \
             patch("amity_harness.cec_pin._ioctl", side_effect=ioctl):
            monitor = PinMonitor(123)
            try:
                self.wait(lambda: queue.empty())
                self.assertIsNone(monitor.receive(.01))
                self.assertIsNone(monitor._error)
                self.assertEqual(monitor.decoder.pending_start_ns, started)
                # Time already passed far beyond protocol limits. The delayed
                # HIGH still has its valid original timestamp and must decode.
                add([*edges[3:], edge(end, False)])
                frame = monitor.receive(.5)
                self.assertEqual(frame.raw, b"\x04\x44\x04")
                self.assertEqual(frame.timestamp_ns, started)
                self.assertIsNone(monitor._error)
            finally:
                monitor.close()


if __name__ == "__main__":
    unittest.main()
