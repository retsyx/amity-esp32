import queue
import sys
import threading
import time
import time as real_time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from unit_tests.virtual_time import (VirtualClock, VirtualClockError, VirtualClockStall, VirtualDeadlock,
                                     use_virtual_time)


class VirtualTimeTests(unittest.TestCase):
    def setUp(self):
        self.started = real_time.perf_counter()
        self.clock = use_virtual_time(self)

    def assertFast(self, limit=2.0):
        self.assertLess(real_time.perf_counter() - self.started, limit)

    def test_reads_and_sleeps_are_virtual_and_only_patched_modules_see_them(self):
        import amity_harness.cec as cec
        self.assertIs(time, self.clock.time)
        self.assertIs(cec.time, self.clock.time)
        self.assertIs(sys.modules["time"], real_time)
        self.assertEqual(self.clock.now, 1000.0)
        time.sleep(3600)
        self.assertEqual(self.clock.now_ns, 4600 * 10**9)
        self.assertLess(time.monotonic_ns(), time.monotonic_ns())  # Successive reads differ.
        self.assertEqual(time.strftime("%Y", time.gmtime(0)), "1970")  # Unpatched names pass through.
        self.clock.close()
        self.assertIs(cec.time, real_time)
        self.assertFast()

    def test_condition_wait_times_out_virtually_and_notify_wakes_it(self):
        condition = threading.Condition()
        with condition:
            self.assertFalse(condition.wait(0.25))
        self.assertAlmostEqual(self.clock.now, 1000.25, places=6)
        woke = []

        def notifier():
            time.sleep(0.1)
            with condition:
                condition.notify()

        thread = threading.Thread(target=notifier)
        thread.start()
        with condition:
            woke.append(condition.wait(5))
        thread.join()
        self.assertEqual(woke, [True])
        self.assertAlmostEqual(self.clock.now, 1000.35, places=6)
        self.assertFast()

    def test_time_cannot_move_while_another_managed_thread_runs(self):
        # The worker burns real CPU without touching the clock. A 1 ms virtual
        # timeout must still see its result: a slow runner never expires it.
        done = threading.Event()

        def worker():
            end = real_time.perf_counter() + 0.05
            while real_time.perf_counter() < end:
                pass
            done.set()

        threading.Thread(target=worker).start()
        self.assertTrue(done.wait(0.001))
        self.assertAlmostEqual(self.clock.now, 1000.0, places=6)

    def test_event_queue_join_and_timer(self):
        event, items = threading.Event(), queue.Queue()
        self.assertFalse(event.wait(1))
        with self.assertRaises(queue.Empty):
            items.get(timeout=2)
        self.assertAlmostEqual(self.clock.now, 1003.0, places=6)
        fired = []
        timer = threading.Timer(0.5, lambda: fired.append(time.monotonic()))
        timer.start()
        cancelled = threading.Timer(0.2, lambda: fired.append("cancelled"))
        cancelled.start()
        cancelled.cancel()
        producer = threading.Thread(target=lambda: (time.sleep(0.25), items.put("item")))
        producer.start()
        self.assertEqual(items.get(timeout=1), "item")
        producer.join()
        timer.join()
        cancelled.join()
        self.assertEqual(len(fired), 1)
        self.assertAlmostEqual(fired[0], 1003.5, places=6)
        self.assertFalse(timer.is_alive())

    def test_join_timeout_and_lock_timeout_follow_virtual_deadlines(self):
        lock, release = threading.Lock(), threading.Event()

        def holder():
            with lock:
                release.wait(1)

        thread = threading.Thread(target=holder)
        thread.start()
        self.clock.settle()
        self.assertFalse(lock.acquire(timeout=0.4))
        self.assertAlmostEqual(self.clock.now, 1000.4, places=6)
        thread.join(0.1)
        self.assertTrue(thread.is_alive())
        self.assertTrue(lock.acquire(timeout=5))
        self.assertAlmostEqual(self.clock.now, 1001.0, places=6)  # Acquired when the holder's wait expired.
        lock.release()
        thread.join()
        self.assertFalse(thread.is_alive())

    def test_rlock_condition_releases_every_level_while_waiting(self):
        lock = threading.RLock()
        condition = threading.Condition(lock)
        seen = []

        def other():
            with lock:
                seen.append(time.monotonic())
                with condition:
                    condition.notify_all()

        with lock, lock:
            thread = threading.Thread(target=other)
            thread.start()
            with condition:
                self.assertTrue(condition.wait(1))
            self.assertTrue(lock._is_owned())
            self.assertTrue(condition._is_owned())
        self.assertFalse(condition._is_owned())
        thread.join()
        self.assertEqual(len(seen), 1)
        self.assertAlmostEqual(seen[0], 1000.0, places=6)

    def test_wait_for_predicate(self):
        condition, state = threading.Condition(), {"ready": False}

        def setter():
            time.sleep(0.3)
            with condition:
                state["ready"] = True
                condition.notify_all()

        threading.Thread(target=setter).start()
        with condition:
            self.assertTrue(condition.wait_for(lambda: state["ready"], timeout=1))
            self.assertFalse(condition.wait_for(lambda: False, timeout=0.2))
        self.assertAlmostEqual(self.clock.now, 1000.5, places=6)

    def test_executor_results_wait_in_virtual_time(self):
        from amity_harness import ble_activity_input
        with ble_activity_input.ThreadPoolExecutor(max_workers=1) as pool:
            future = pool.submit(lambda: (time.sleep(2), "settled")[1])
            self.assertEqual(future.result(), "settled")
            failing = pool.submit(lambda: 1 / 0)
            with self.assertRaises(ZeroDivisionError):
                failing.result()
        self.assertAlmostEqual(self.clock.now, 1002.0, places=6)

    def test_advance_wakes_deadlines_it_passes(self):
        woke = []
        thread = threading.Thread(target=lambda: woke.append(threading.Event().wait(10)))
        thread.start()
        self.clock.settle()
        self.clock.advance(10)
        thread.join()
        self.assertEqual(woke, [False])
        self.assertAlmostEqual(self.clock.now, 1010.0, places=6)

    def test_deadlock_is_reported_instead_of_hanging(self):
        self.clock.deadlock_grace_seconds = 0.05
        with self.assertRaises(VirtualDeadlock) as raised:
            threading.Event().wait()
        self.assertIn("waits forever", str(raised.exception))

    def test_busy_loop_without_waiting_is_reported(self):
        self.clock.spin_limit = 1000
        with self.assertRaises(VirtualClockStall):
            deadline = time.monotonic() + 1
            while time.monotonic() < deadline:
                pass

    def test_stall_in_a_real_call_is_reported(self):
        self.clock.stall_seconds = 0.2
        real = real_time.sleep
        threading.Thread(target=lambda: real(0.6), daemon=True).start()
        with self.assertRaises(VirtualClockStall):
            threading.Event().wait(1)

    def test_unmanaged_threads_cannot_wait(self):
        event, errors = threading.Event(), []

        def unmanaged():
            try:
                event.wait(1)
            except VirtualClockError as error:
                errors.append(error)

        thread = self.clock.threading._real.Thread(target=unmanaged)
        thread.start()
        thread.join()
        self.assertEqual(len(errors), 1)

    def test_threads_left_waiting_after_close_continue_in_real_time(self):
        clock = VirtualClock()
        stop = clock.Event()
        thread = clock.Thread(target=lambda: stop.wait(30), daemon=True)
        thread.start()
        clock.close()
        real_start = real_time.monotonic()
        stop.set()
        thread.join(5)
        self.assertFalse(thread.is_alive())
        self.assertLess(real_time.monotonic() - real_start, 2)


if __name__ == "__main__":
    unittest.main()
