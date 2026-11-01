"""One AVR state across independent physical CEC attachments."""

from pathlib import Path
import sys
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import (AvrAttachment, AvrComponent, CecAddress, CecSession,
                           DUT, Frame, PlaybackComponent, PowerState, TvComponent)
from unit_tests.test_virtual_bus import Backend


class AvrAttachmentTests(unittest.TestCase):
    def setUp(self):
        self.dut = DUT("amity-dut-4")
        self.tv = TvComponent(self.dut.tentacle(1).front, "Attached TV")
        self.avr = AvrComponent(self.dut.tentacle(1).back, "Attached AVR")
        self.player = PlaybackComponent(self.avr, "Attached Play", port=1)
        self.input = AvrAttachment(self.avr, self.dut.tentacle(2).front)
        self.thief = PlaybackComponent(self.dut.tentacle(2).back, "Attached Thief",
                                       address=CecAddress(4, "1.2.0.0"))
        self.components = (self.tv, self.avr, self.player, self.input, self.thief)
        self.backends = {}

    def backend(self, gpio):
        backend = Backend(gpio)
        self.backends[gpio] = backend
        return backend

    def session(self):
        return CecSession(self.components, backend_factory=self.backend)

    def frame(self, opcode, operands=b"", destination=15):
        return Frame(8, destination, opcode, operands, timestamp_ns=time.monotonic_ns())

    def test_identity_and_state_are_shared_but_frames_and_evidence_are_not(self):
        with self.session() as session:
            self.assertEqual(self.avr.name, self.input.name)
            self.assertEqual(self.avr.address, self.input.address)
            self.assertEqual(self.avr.address.physical, 0x1000)
            self.assertEqual(self.thief.address.physical, 0x1200)
            self.assertNotEqual(self.avr.gpio, self.input.gpio)
            for backend in self.backends.values():
                backend.sent.clear()
            self.avr.power_on()
            self.assertEqual(self.input.power, PowerState.ON)
            self.input.receive(self.frame(0x82, b"\x12\x00"))
            self.assertEqual(self.avr.active_input, 0x1200)
            self.assertEqual(self.input.active_input, self.avr.active_input)
            self.assertEqual(len(self.input.routing_events), 1)
            self.assertEqual(self.avr.routing_events, ())
            self.assertFalse(any(backend.sent for backend in self.backends.values()))
            self.avr.receive(self.frame(0x86, b"\x11\x00"))
            self.assertEqual(self.input.active_input, 0x1100)
            self.input.receive(self.frame(0x36, destination=5))
            self.assertEqual(self.avr.power, PowerState.STANDBY)
            self.assertEqual(self.input.power, PowerState.STANDBY)
            self.assertEqual(len(self.input.power_events), 1)
            self.assertEqual(self.avr.power_events, ())
            self.assertFalse(any(backend.sent for backend in self.backends.values()))

    def test_discovery_replies_use_the_receiving_attachment_wire(self):
        with self.session():
            for backend in self.backends.values():
                backend.sent.clear()
            self.input.receive(self.frame(0x46, destination=5))
            self.assertEqual([frame.opcode for frame in self.backends[self.input.gpio].sent], [0x47])
            self.assertFalse(self.backends[self.avr.gpio].sent)

    def test_power_off_and_on_update_both_electrical_ack_masks(self):
        with self.session():
            self.avr.power_off()
            for component in (self.avr, self.input):
                self.assertEqual(component.power, PowerState.OFF)
                self.assertNotIn(component, self.backends[component.gpio].claimed)
            self.input.power_on()
            for component in (self.avr, self.input):
                self.assertEqual(component.power, PowerState.ON)
                self.assertIn(component, self.backends[component.gpio].claimed)

    def test_attachment_cannot_claim_a_different_identity_or_same_wire(self):
        with self.assertRaisesRegex(ValueError, "logical and physical"):
            AvrAttachment(self.avr, self.dut.tentacle(2).front, address=CecAddress(5, "2.0.0.0"))
        with self.assertRaisesRegex(ValueError, "already has an attachment"):
            AvrAttachment(self.avr, self.avr.endpoint)

    def test_attachment_cannot_run_without_its_original_avr_in_the_session(self):
        with self.assertRaisesRegex(ValueError, "same CEC session"):
            with CecSession((self.input,), backend_factory=self.backend):
                pass
        self.assertTrue(all(backend.closed for backend in self.backends.values()))


if __name__ == "__main__":
    unittest.main()
