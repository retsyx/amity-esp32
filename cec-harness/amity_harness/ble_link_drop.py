"""Pi-only helper that ends a peer's next BLE links as soon as they exist.

The keyboard worker runs this with the CAP_NET_RAW interpreter that harness
setup installs. It reads controller events from a raw HCI socket and sends
HCI Disconnect the moment the peer's connection is reported, before the
peer's Bluetooth host can finish its own connection setup. It uses only the
standard library so it can run isolated (python -I).
"""

import argparse
import json
import re
import select
import socket
import struct
import sys
import time


HCI_EVENT = 0x04
HCI_COMMAND = 0x01
EVENT_DISCONNECTION_COMPLETE = 0x05
EVENT_COMMAND_STATUS = 0x0F
EVENT_LE_META = 0x3E
LE_CONNECTION_COMPLETE = 0x01
LE_LONG_TERM_KEY_REQUEST = 0x05
LE_ENHANCED_CONNECTION_COMPLETE = 0x0A
LE_ENHANCED_CONNECTION_COMPLETE_V2 = 0x29
OPCODE_DISCONNECT = 0x0406
ROLE_PERIPHERAL = 0x01
REMOTE_USER_TERMINATED = 0x13
HCI_CMSG_TSTAMP = 0x0002
# Disconnection Complete, Command Status and LE Meta events only.
EVENT_MASK = ((1 << EVENT_DISCONNECTION_COMPLETE) | (1 << EVENT_COMMAND_STATUS), 1 << (EVENT_LE_META - 32))
DISCONNECT_TIMEOUT_NS = 5_000_000_000


def hci_filter():
    return struct.pack("<IIIH2x", 1 << HCI_EVENT, *EVENT_MASK, 0)


def _address(raw):
    return ":".join(f"{byte:02X}" for byte in reversed(raw))


def parse_event(packet):
    """Decode only the events this helper needs; ignore everything else."""
    if len(packet) < 3 or packet[0] != HCI_EVENT or len(packet) != 3 + packet[2]:
        return None
    code, params = packet[1], packet[3:]
    if code == EVENT_DISCONNECTION_COMPLETE and len(params) == 4:
        return ("disconnected", int.from_bytes(params[1:3], "little") & 0x0FFF, params[0], params[3])
    if code == EVENT_COMMAND_STATUS and len(params) == 4:
        return ("command_status", int.from_bytes(params[2:4], "little"), params[0])
    if code != EVENT_LE_META or not params:
        return None
    if params[0] in (LE_CONNECTION_COMPLETE, LE_ENHANCED_CONNECTION_COMPLETE,
                     LE_ENHANCED_CONNECTION_COMPLETE_V2) and len(params) >= 12:
        return ("connected", int.from_bytes(params[2:4], "little") & 0x0FFF, params[1], params[4],
                _address(params[6:12]))
    if params[0] == LE_LONG_TERM_KEY_REQUEST and len(params) >= 3:
        return ("ltk_request", int.from_bytes(params[1:3], "little") & 0x0FFF)
    return None


def disconnect_command(handle):
    return struct.pack("<BHBHB", HCI_COMMAND, OPCODE_DISCONNECT, 3, handle, REMOTE_USER_TERMINATED)


class HciSocket:
    """Raw HCI events with kernel receive timestamps (CLOCK_REALTIME)."""

    def __init__(self, index):
        self.socket = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_RAW, socket.BTPROTO_HCI)
        try:
            self.socket.setsockopt(socket.SOL_HCI, socket.HCI_FILTER, hci_filter())
            self.socket.setsockopt(socket.SOL_HCI, socket.HCI_TIME_STAMP, 1)
            self.socket.bind((index,))
            # Without CAP_NET_RAW the kernel silently removes LE events.
            applied = self.socket.getsockopt(socket.SOL_HCI, socket.HCI_FILTER, 16)
            if struct.unpack_from("<IIIH", applied) != struct.unpack_from("<IIIH", hci_filter()):
                raise PermissionError("Reading LE connection events requires CAP_NET_RAW")
        except BaseException:
            self.socket.close()
            raise

    def receive(self, timeout):
        ready, _, _ = select.select([self.socket], [], [], max(0, timeout))
        if not ready:
            return None
        packet, ancillary, _, _ = self.socket.recvmsg(260, socket.CMSG_SPACE(16))
        stamp = None
        for level, kind, value in ancillary:
            if level == socket.SOL_HCI and kind == HCI_CMSG_TSTAMP and len(value) >= 16:
                seconds, micros = struct.unpack("<qq", value[:16])
                stamp = seconds * 1_000_000_000 + micros * 1000
        return packet, stamp

    def send(self, packet):
        self.socket.send(packet)

    def close(self):
        self.socket.close()


def drop_links(link, peer, count, timeout_ns, emit, clock=time.time_ns):
    """End the peer's next count links; return whether all of them arrived."""
    peer = peer.upper()
    for number in range(1, count + 1):
        deadline = clock() + timeout_ns
        while True:
            received = link.receive((deadline - clock()) / 1e9)
            if received is None:
                emit({"error": "no new link from the peer", "drops": number - 1})
                return False
            event = parse_event(received[0])
            if (event is not None and event[0] == "connected" and event[2] == 0
                    and event[3] == ROLE_PERIPHERAL and event[4] == peer):
                connected = received[1] if received[1] is not None else clock()
                handle = event[1]
                break
        link.send(disconnect_command(handle))
        sent = clock()
        status = reason = disconnected = None
        encrypted = False
        deadline = sent + DISCONNECT_TIMEOUT_NS
        while status is None or reason is None:
            received = link.receive((deadline - clock()) / 1e9)
            if received is None:
                emit({"error": "the dropped link did not end", "drops": number - 1})
                return False
            event = parse_event(received[0])
            if event is None:
                continue
            if event[0] == "command_status" and event[1] == OPCODE_DISCONNECT and status is None:
                status = event[2]
            elif event[0] == "ltk_request" and event[1] == handle and reason is None:
                # Only the central starts encryption, and only after its host
                # has reported the link; the drop came too late.
                encrypted = True
            elif event[0] == "disconnected" and event[1] == handle and event[2] == 0 and reason is None:
                reason = event[3]
                disconnected = received[1] if received[1] is not None else clock()
        emit({"drop": {"number": number, "handle": handle, "reaction_ns": sent - connected,
                       "link_ns": disconnected - connected, "command_status": status,
                       "reason": reason, "encrypted": encrypted}})
    return True


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adapter", required=True, help="BlueZ adapter, for example hci0")
    parser.add_argument("--peer", required=True, help="the peer's address, for example AA:BB:CC:DD:EE:FF")
    parser.add_argument("--count", type=int, required=True, help="how many new links to end (1-5)")
    parser.add_argument("--timeout", type=int, required=True, help="seconds to wait for each new link (5-60)")
    args = parser.parse_args(argv)
    adapter = re.fullmatch(r"hci([0-9]+)", args.adapter)
    if (adapter is None or re.fullmatch(r"[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}", args.peer) is None
            or not 1 <= args.count <= 5 or not 5 <= args.timeout <= 60):
        parser.error("Choose an hci adapter, a peer address, 1-5 links and a 5-60 second timeout")

    def emit(record):
        print(json.dumps(record, separators=(",", ":")), flush=True)

    try:
        link = HciSocket(int(adapter[1]))
    except OSError as error:
        print("BLE link drop: " + str(error), file=sys.stderr)
        return 1
    try:
        emit({"armed": True})
        return 0 if drop_links(link, args.peer, args.count, args.timeout * 1_000_000_000, emit) else 1
    finally:
        link.close()


if __name__ == "__main__":
    raise SystemExit(main())
