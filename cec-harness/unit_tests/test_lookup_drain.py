"""Lookup no-scan evidence must drain receive FIFOs behind synchronous answers.

Responders answer each query with a wire transmission under the session lock,
so a burst of identity questions delays the proof that every witness wire's
receive FIFO emptied until those answers are on the wire.
"""

from contextlib import contextmanager
from pathlib import Path
import queue
import sys
import threading
import time
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import (AvrComponent, CecSession, DUT, Frame, PlaybackComponent, PowerState,
                           TvComponent, TxResult)
from amity_harness.config import RunConfig, use_config
from amity_harness.recovery import LookupScans
from unit_tests.virtual_time import use_virtual_time


BOOT = "b" * 32
GENERATION = 64
# The DUT's initiators on the adapter's front (TV) and back (AVR) wires.
FRONT_INITIATOR, BACK_INITIATOR = 4, 0


class TimedWire:
    """A cec-gpio stand-in with a kernel receive FIFO and CEC frame timing.

    A transmission returns once its signal free time, start bit and 24 ms
    per byte have elapsed. DUT frames wait in the FIFO, stamped with their
    receive time, until the session worker polls them.
    """

    def __init__(self, gpio):
        self.gpio = gpio
        self.fifo = queue.Queue()
        self.sent = []

    def open(self):
        pass

    def configure(self, components):
        pass

    def close(self):
        pass

    def transmit(self, frame):
        time.sleep(.0168 + .0045 + .024 * len(frame.raw))
        result = TxResult(frame, 1, time.monotonic_ns())
        self.sent.append(result)
        return result

    def receive(self, timeout):
        try:
            return self.fifo.get(timeout=timeout)
        except queue.Empty:
            return None

    def dut_sends(self, *frames):
        for frame in frames:
            self.fifo.put(Frame(frame.source, frame.destination, frame.opcode, frame.operands,
                                timestamp_ns=time.monotonic_ns()))


def questions(initiator, component, opcodes=(0x83, 0x46, 0x8c)):
    return [Frame(initiator, component.address.logical, opcode) for opcode in opcodes]


class LookupDrainTests(unittest.TestCase):
    def setUp(self):
        self.clock = use_virtual_time(self)
        self.wires = {}

    def wire(self, gpio):
        self.wires[gpio] = TimedWire(gpio)
        return self.wires[gpio]

    @contextmanager
    def observation(self, *, player=PowerState.OFF, config=None, state=None):
        """The manual-component wiring: TV in front, AVR and player behind."""
        with use_config(config or RunConfig()):
            dut = DUT("amity-dut")
        tentacle = dut.tentacle(0)
        tv = TvComponent(tentacle.front, "Manual TV")
        avr = AvrComponent(tentacle.back, "Manual AVR")
        source = PlaybackComponent(avr, "Manual Player", port=1, power=player)
        with CecSession([tv, avr, source], backend_factory=self.wire) as session:
            scan = SimpleNamespace(_session=session, dut=dut, _boot=BOOT, _testbed=SimpleNamespace(),
                                   status={"discovery_generation": GENERATION},
                                   owner_boots={dut.identity.device_id: BOOT},
                                   owner_duts={dut.identity.device_id: dut},
                                   initiators={tv.gpio: FRONT_INITIATOR, avr.gpio: BACK_INITIATOR},
                                   validate_owners=lambda components=None: None)
            dut.last_scan = scan
            dut.get_state = lambda: {"expected_boot_id": BOOT, "discovering": False,
                                     "discovery_generation": GENERATION, **(state or {})}
            with LookupScans(scan, tv, avr) as observer:
                yield observer, session, tv, avr, source

    def later(self, wire, *frames):
        """The DUT sends ``frames`` one millisecond after the evidence boundary.

        Virtual time stands still until the checking thread waits, which it
        first does after taking its boundary.
        """
        def send():
            time.sleep(.001)
            wire.dut_sends(*frames)

        thread = threading.Thread(target=send)
        thread.start()
        self.addCleanup(thread.join)

    def repeat(self, wire, frame, interval):
        """The DUT keeps sending ``frame`` until the test ends."""
        stop = threading.Event()

        def send():
            while not stop.is_set():
                wire.dut_sends(frame)
                stop.wait(interval)

        thread = threading.Thread(target=send)
        thread.start()
        self.addCleanup(thread.join)
        self.addCleanup(stop.set)

    @staticmethod
    def check(observer):
        return observer.scan._testbed.control_observations[-1]["checks"][-1]

    def test_returned_player_identity_answers_do_not_consume_the_drain_timeout(self):
        # The bench return: re-announcing the back wire makes the DUT ask the
        # player and the AVR for their identities, then ask again. Answering
        # takes 2.2 s of wire time, so the AVR's wire cannot poll empty
        # within one CEC timeout of the boundary.
        with self.observation() as (observer, session, tv, avr, player):
            player.standby()
            back = self.wires[avr.gpio]
            asked = [*questions(BACK_INITIATOR, player), *questions(BACK_INITIATOR, avr),
                     *questions(BACK_INITIATOR, player), *questions(BACK_INITIATOR, avr, (0x46,))]
            answered = len(back.sent)
            self.later(back, *asked)
            observer.assert_no_scan_on_witnesses()

            check = self.check(observer)
            through = check["through_monotonic_ns"]
            answers = back.sent[answered:]
            self.assertEqual(len(answers), len(asked))
            self.assertGreater(answers[-1].timestamp_ns - through, 2 * 10**9)
            # Coverage is still an empty poll begun after the boundary on both
            # witness wires; on the AVR's wire it came only after the last answer.
            self.assertGreaterEqual(session._wire_idle_since_ns[tv.gpio], through)
            self.assertGreaterEqual(session._wire_idle_since_ns[avr.gpio], answers[-1].timestamp_ns)
            self.assertTrue(check["passed"])
            self.assertEqual(check["unexpected_queries"], [])
            # The AVR's questions follow its fresh announcement within one CEC timeout.
            self.assertEqual([query["query_frame"] for query in check["metadata_queries"]],
                             ["05:83", "05:46", "05:46"])

    def test_scan_behind_queued_answers_still_fails(self):
        cases = {
            # Identity requests to an unannounced witness, received before or
            # after the boundary, queue behind answers that outlast the timeout.
            "witness_queries_before_boundary": ({}, "unsolicited lookup identity queries"),
            "witness_queries_after_boundary": ({}, "unsolicited lookup identity queries"),
            # Polls across every address while the UI reports a new scan.
            "address_polls": ({"discovery_generation": GENERATION + 1}, "unsolicited discovery"),
        }
        for name, (state, message) in cases.items():
            with self.subTest(name), self.observation(player=PowerState.STANDBY, state=state) as (
                    observer, session, tv, avr, player):
                back = self.wires[avr.gpio]
                busy = 2 * questions(BACK_INITIATOR, player)
                if name == "address_polls":
                    scanned = [Frame(BACK_INITIATOR, address) for address in range(1, 15)]
                else:
                    scanned = questions(BACK_INITIATOR, avr, (0x83, 0x46))
                if name == "witness_queries_before_boundary":
                    back.dut_sends(*busy, *scanned)
                else:
                    self.later(back, *busy, *scanned)
                with self.assertRaisesRegex(AssertionError, message):
                    observer.assert_no_scan_on_witnesses()

                check = self.check(observer)
                self.assertFalse(check["passed"])
                if name != "address_polls":
                    self.assertEqual([query["query_frame"] for query in check["unexpected_queries"]],
                                     ["05:83", "05:46"])
                if name == "witness_queries_before_boundary":
                    self.assertTrue(all(query["query_wire_timestamp_ns"] < check["through_monotonic_ns"]
                                        for query in check["unexpected_queries"]))

    def test_unanswered_traffic_still_fails_one_cec_timeout_after_the_boundary(self):
        # The FIFO never empties, but the harness transmits nothing.
        with self.observation(player=PowerState.STANDBY) as (observer, session, tv, avr, player):
            self.repeat(self.wires[avr.gpio], Frame(BACK_INITIATOR, 15, 0x87, b"\x00\x00\x02"), .02)
            started = time.monotonic()
            with self.assertRaisesRegex(TimeoutError, "did not drain"):
                observer.assert_no_scan_on_witnesses()
            self.assertLess(time.monotonic() - started, 1.1)

    def test_endless_answers_fail_at_the_operation_timeout(self):
        # Every 120 ms the DUT asks for a status the AVR answers in 93 ms, so
        # the FIFO never stays empty for a whole 50 ms poll.
        with self.observation(player=PowerState.STANDBY, config=RunConfig(timeout=3)) as (
                observer, session, tv, avr, player):
            self.repeat(self.wires[avr.gpio], Frame(BACK_INITIATOR, avr.address.logical, 0x8f), .12)
            started = time.monotonic()
            with self.assertRaisesRegex(TimeoutError, "did not drain"):
                observer.assert_no_scan_on_witnesses()
            elapsed = time.monotonic() - started
            self.assertGreaterEqual(elapsed, 3)
            self.assertLess(elapsed, 3.2)


if __name__ == "__main__":
    unittest.main()
