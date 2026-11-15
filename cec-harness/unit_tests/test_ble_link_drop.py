"""Raw HCI link-drop decisions without a Bluetooth controller."""

from contextlib import redirect_stderr, redirect_stdout
import io
from pathlib import Path
import struct
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import ble_link_drop
from amity_harness.ble_link_drop import (OPCODE_DISCONNECT, HciSocket, disconnect_command, drop_links,
                                         hci_filter, main, parse_event)


PEER = "AA:BB:CC:DD:EE:01"


def connected(handle, *, status=0, role=1, peer=PEER, subevent=0x01):
    address = bytes(reversed(bytes.fromhex(peer.replace(":", ""))))
    tail = bytes(7) if subevent == 0x01 else bytes(19 if subevent == 0x0A else 22)
    params = bytes([subevent, status]) + handle.to_bytes(2, "little") + bytes([role, 0]) + address + tail
    return bytes([0x04, 0x3E, len(params)]) + params


def disconnected(handle, reason=0x13, status=0):
    return bytes([0x04, 0x05, 4, status]) + handle.to_bytes(2, "little") + bytes([reason])


def command_status(status=0, opcode=OPCODE_DISCONNECT):
    return bytes([0x04, 0x0F, 4, status, 1]) + opcode.to_bytes(2, "little")


def ltk_request(handle):
    params = bytes([0x05]) + handle.to_bytes(2, "little") + bytes(10)
    return bytes([0x04, 0x3E, len(params)]) + params


class FakeLink:
    """Controller events in order; each carries a kernel receive time."""

    def __init__(self, *events):
        self.events, self.sent, self.log = list(events), [], []

    def receive(self, timeout):
        if not self.events:
            return None
        packet, stamp = self.events.pop(0)
        self.log.append(("receive", packet))
        return packet, stamp

    def send(self, packet):
        self.sent.append(packet)
        self.log.append(("send", packet))


class LinkDropTests(unittest.TestCase):
    def test_filter_admits_only_disconnection_command_status_and_le_events(self):
        types, low, high, opcode = struct.unpack("<IIIH2x", hci_filter())
        self.assertEqual((types, low, high, opcode), (1 << 4, (1 << 5) | (1 << 15), 1 << 30, 0))

    def test_events_decode_handles_roles_and_addresses(self):
        for subevent in (0x01, 0x0A, 0x29):
            with self.subTest(subevent=subevent):
                self.assertEqual(parse_event(connected(0x1041, subevent=subevent)),
                                 ("connected", 0x041, 0, 1, PEER))
        self.assertEqual(parse_event(connected(0x40, status=0x3E, role=0)), ("connected", 0x40, 0x3E, 0, PEER))
        self.assertEqual(parse_event(disconnected(0x40, 0x08)), ("disconnected", 0x40, 0, 0x08))
        self.assertEqual(parse_event(command_status(0x02)), ("command_status", OPCODE_DISCONNECT, 0x02))
        self.assertEqual(parse_event(ltk_request(0x40)), ("ltk_request", 0x40))
        for packet in (b"", bytes([0x04, 0x05, 4, 0]), connected(0x40)[:-1], bytes([0x02, 0x05, 0]),
                       bytes([0x04, 0x3E, 1, 0x02]), bytes([0x04, 0x0E, 1, 0])):
            with self.subTest(packet=packet.hex()):
                self.assertIsNone(parse_event(packet))

    def test_disconnect_command_is_remote_user_termination(self):
        self.assertEqual(disconnect_command(0x0041), bytes.fromhex("01 06 04 03 41 00 13"))

    def test_only_the_peers_new_peripheral_links_are_dropped_at_once(self):
        link = FakeLink((connected(0x40, peer="AA:BB:CC:DD:EE:02"), 1),
                        (connected(0x41, role=0), 2),
                        (connected(0x42, status=0x3E), 3),
                        (connected(0x43), 1_000_000),
                        (command_status(), 1_100_000),
                        (disconnected(0x43), 31_000_000))
        records = []
        self.assertTrue(drop_links(link, PEER.lower(), 1, 10**9, records.append, clock=lambda: 1_500_000))
        self.assertEqual(link.sent, [disconnect_command(0x43)])
        # The command goes out before anything after the connection is read.
        self.assertEqual(link.log[4], ("send", disconnect_command(0x43)))
        self.assertEqual(records, [{"drop": {"number": 1, "handle": 0x43, "reaction_ns": 500_000,
                                             "link_ns": 30_000_000, "command_status": 0, "reason": 0x13,
                                             "encrypted": False}}])

    def test_encryption_before_link_loss_marks_a_late_drop(self):
        link = FakeLink((connected(0x40), 10), (ltk_request(0x40), 11), (command_status(), 12),
                        (disconnected(0x40), 13))
        records = []
        self.assertTrue(drop_links(link, PEER, 1, 10**9, records.append, clock=lambda: 20))
        self.assertIs(records[0]["drop"]["encrypted"], True)

    def test_a_link_that_ends_before_the_command_is_still_one_drop(self):
        link = FakeLink((connected(0x40), 10), (disconnected(0x40, 0x08), 12), (command_status(0x02), 13),
                        (connected(0x41), 20), (command_status(), 21), (disconnected(0x41), 22))
        records = []
        self.assertTrue(drop_links(link, PEER, 2, 10**9, records.append, clock=lambda: 15))
        self.assertEqual([(row["drop"]["handle"], row["drop"]["reason"], row["drop"]["command_status"])
                          for row in records], [(0x40, 0x08, 0x02), (0x41, 0x13, 0)])

    def test_a_peer_that_stops_reconnecting_ends_the_helper_with_its_count(self):
        link = FakeLink((connected(0x40), 10), (command_status(), 11), (disconnected(0x40), 12))
        records = []
        self.assertFalse(drop_links(link, PEER, 3, 10**9, records.append, clock=lambda: 15))
        self.assertEqual(records[-1], {"error": "no new link from the peer", "drops": 1})
        lingering = FakeLink((connected(0x40), 10), (command_status(), 11))
        records.clear()
        self.assertFalse(drop_links(lingering, PEER, 1, 10**9, records.append, clock=lambda: 15))
        self.assertEqual(records, [{"error": "the dropped link did not end", "drops": 0}])

    def test_arguments_are_bounded_before_opening_the_adapter(self):
        for argv in (["--adapter", "bluetooth0", "--peer", PEER, "--count", "1", "--timeout", "20"],
                     ["--adapter", "hci0", "--peer", "AA:BB", "--count", "1", "--timeout", "20"],
                     ["--adapter", "hci0", "--peer", PEER, "--count", "6", "--timeout", "20"],
                     ["--adapter", "hci0", "--peer", PEER, "--count", "1", "--timeout", "61"]):
            with self.subTest(argv=argv), patch.object(ble_link_drop, "HciSocket") as opened, \
                    redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                main(argv)
            opened.assert_not_called()

    def test_main_reports_armed_then_each_drop(self):
        link = FakeLink((connected(0x40), 10), (command_status(), 11), (disconnected(0x40), 12))
        link.close = lambda: link.log.append(("close",))
        output = io.StringIO()
        with patch.object(ble_link_drop, "HciSocket", return_value=link) as opened, redirect_stdout(output):
            self.assertEqual(main(["--adapter", "hci1", "--peer", PEER, "--count", "1", "--timeout", "20"]), 0)
        opened.assert_called_once_with(1)
        lines = output.getvalue().splitlines()
        self.assertEqual(lines[0], '{"armed":true}')
        self.assertIn('"handle":64', lines[1])
        self.assertEqual(link.log[-1], ("close",))

    def test_socket_requires_net_raw_and_reads_kernel_timestamps(self):
        class Socket:
            def __init__(self, applied):
                self.applied, self.closed = applied, False

            def setsockopt(self, *args):
                pass

            def bind(self, address):
                self.address = address

            def getsockopt(self, *args):
                return self.applied

            def recvmsg(self, size, ancillary):
                return b"\x04\x0f", [(0, 2, struct.pack("<qq", 3, 250))], 0, None

            def close(self):
                self.closed = True

        masked = Socket(struct.pack("<IIIH2x", 1 << 4, (1 << 5) | (1 << 15), 0, 0))
        module = SimpleNamespace(AF_BLUETOOTH=31, SOCK_RAW=3, BTPROTO_HCI=1, SOL_HCI=0, HCI_FILTER=2,
                                 HCI_TIME_STAMP=3, CMSG_SPACE=lambda size: size + 16,
                                 socket=lambda *args: masked)
        with patch.object(ble_link_drop, "socket", module), self.assertRaisesRegex(PermissionError, "CAP_NET_RAW"):
            HciSocket(0)
        self.assertTrue(masked.closed)
        granted = Socket(hci_filter())
        module.socket = lambda *args: granted
        with patch.object(ble_link_drop, "socket", module), \
                patch.object(ble_link_drop.select, "select", return_value=([granted], [], [])):
            link = HciSocket(0)
            self.assertEqual(granted.address, (0,))
            self.assertEqual(link.receive(1), (b"\x04\x0f", 3_000_250_000))


if __name__ == "__main__":
    unittest.main()
