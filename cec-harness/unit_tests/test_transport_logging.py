"""Trace provenance and assertion progress without physical DUT access."""

import logging
from pathlib import Path
import sys
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import (AvrComponent, CecSession, DUT, Frame,
                           PlaybackComponent, TvComponent, TxResult, assign_addresses)


class IdleWire:
    def __init__(self, gpio):
        self.gpio = gpio
        self.idle = threading.Event()

    def open(self):
        pass

    def configure(self, components):
        pass

    def transmit(self, frame):
        return TxResult(frame, 1, time.monotonic_ns())

    def receive(self, timeout):
        self.idle.wait(timeout)
        return None

    def close(self):
        self.idle.set()


class TransportLoggingTests(unittest.TestCase):
    def topology(self):
        dut = DUT("amity-dut")
        tv = TvComponent(dut.tentacle(0).front, "Trace TV")
        avr = AvrComponent(dut.tentacle(0).back, "Trace AVR")
        player = PlaybackComponent(avr, "Trace Player", port=1)
        return dut, tv, avr, player

    def test_frame_trace_survives_evidence_ring_eviction_and_keeps_provenance(self):
        dut, tv, avr, player = self.topology()
        assign_addresses([tv, avr, player])
        session = CecSession([tv, avr, player], max_records=1)
        frame = Frame(0, avr.address.logical, 0x36, timestamp_ns=1234)
        with patch("amity_harness.cec.event") as emit:
            session._record(avr, "rx", frame)
            session._record(player, "tx", frame, transport="virtual", status=1, ok=True)
            session._record(avr, "tx", frame, status=0, ok=False,
                            error="backend echoed private-session-token")
        self.assertEqual(len(session.records), 1)
        rows = [call.kwargs for call in emit.call_args_list]
        self.assertEqual(len(rows), 3)
        self.assertTrue(all(row["level"] == logging.DEBUG for row in rows))
        self.assertEqual([row["transport"] for row in rows], ["wire", "virtual", "wire"])
        self.assertEqual([row["wire_timestamp_ns"] for row in rows], [1234, 0, 1234])
        self.assertEqual(rows[0]["dut"], dut.name)
        self.assertEqual(rows[0]["gpio"], avr.gpio)
        self.assertEqual(rows[0]["tentacle"], 0)
        self.assertEqual(rows[0]["side"], "back")
        self.assertEqual(rows[0]["frame"], frame.raw.hex(":"))
        self.assertNotIn("private-session-token", str(rows))

    def test_component_trace_explains_power_and_routing_assertion_evidence(self):
        dut, tv, avr, player = self.topology()
        with patch("amity_harness.components.trace_event") as emit:
            with CecSession([tv, avr, player], backend_factory=IdleWire) as session:
                dut.last_scan = SimpleNamespace(initiators={tv.gpio: 4, avr.gpio: 0}, _session=session)
                action = SimpleNamespace(dut=dut, session=session, started_ns=time.monotonic_ns(),
                                         initiators=dict(dut.last_scan.initiators))
                tv.assert_power("standby")
                tv.receive(Frame(4, tv.address.logical, 0x04, timestamp_ns=time.monotonic_ns()))
                tv.wait_for_power("on", since=action)
                avr.receive(Frame(0, 15, 0x86, player.address.physical.to_bytes(2, "big"),
                                  timestamp_ns=time.monotonic_ns()))
                avr.wait_for_input(player, since=action)
        events = [(call.args[0], call.kwargs) for call in emit.call_args_list]
        power = next(fields for name, fields in events
                     if name == "component.power.assert_passed" and fields["kind"] == "wire_command")
        route = next(fields for name, fields in events if name == "component.routing.assert_passed")
        self.assertEqual(power["dut"], dut.name)
        self.assertEqual(power["component"], tv.name)
        self.assertEqual(power["side"], "front")
        self.assertEqual(power["power"], "on")
        self.assertEqual(power["since_monotonic_ns"], action.started_ns)
        self.assertGreaterEqual(power["wire_timestamp_ns"], action.started_ns)
        self.assertEqual(route["side"], "back")
        self.assertEqual(route["physical_address"], player.address.physical)
        self.assertEqual(route["command"], "set_stream_path")
        names = [name for name, _ in events]
        self.assertLess(names.index("component.power.wait"),
                        next(index for index, (name, fields) in enumerate(events)
                             if name == "component.power.assert_passed" and fields["kind"] == "wire_command"))
        self.assertLess(names.index("component.routing.wait"), names.index("component.routing.assert_passed"))


if __name__ == "__main__":
    unittest.main()
