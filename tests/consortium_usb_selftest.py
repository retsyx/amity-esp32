#!/usr/bin/env python3
"""Run the offline DUT crypto self-test; no provision/reset/CEC commands.

Use the pinned IDF Python environment (pyserial). The full diagnostic stream may
contain private device output, so it is written only to a new mode0600 file and
never echoed. The port opens with DTR/RTS already false, preserving boot state.
"""
import argparse
import os
from pathlib import Path
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--private-log', type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=60)
    args = parser.parse_args()
    if not 1 <= args.timeout <= 120:
        parser.error('Choose a bounded timeout of 1–120 seconds')
    import serial
    fd = os.open(args.private_log, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'wb', buffering=0) as log:
        device = serial.Serial(port=None, baudrate=115200, timeout=0.2, exclusive=True)
        device.dtr = False
        device.rts = False
        device.port = args.port
        device.open()
        with device as port:
            rebooted = False
            for command, prefix, timeout in [('AMITY CONSORTIUM ID', b'AMITY CONSORTIUM ID ', 10),
                                             ('AMITY CONSORTIUM SELFTEST', b'AMITY CONSORTIUM SELFTEST ', args.timeout)]:
                port.write(('\n' + command + '\n').encode())
                deadline = time.monotonic() + timeout
                next_id_retry = time.monotonic() + 2
                while time.monotonic() < deadline:
                    raw = port.readline()
                    if prefix.endswith(b'ID ') and time.monotonic() >= next_id_retry:
                        port.write(('\n' + command + '\n').encode())
                        next_id_retry = time.monotonic() + 2
                    if raw:
                        log.write(raw)
                    line = raw.strip()
                    if b'ESP-ROM:' in line or b'Guru Meditation' in line or b'Backtrace:' in line:
                        rebooted = True
                    if line.startswith(prefix):
                        if prefix.endswith(b'ID '):
                            print(line.decode('ascii', errors='replace'), flush=True)
                            if rebooted:
                                print('Port-open boot observed before ID; retained in private capture', flush=True)
                            rebooted = False
                        else:
                            passed = line == prefix + b'OK' and not rebooted
                            print('Consortium DUT crypto SELFTEST ' + ('PASS' if passed else 'FAIL'), flush=True)
                            if not passed:
                                raise SystemExit(1)
                        break
                else:
                    print(command + ' TIMEOUT' + (' (reboot/crash observed)' if rebooted else ''), flush=True)
                    raise SystemExit(1)
    print('Private diagnostic capture: ' + str(args.private_log))


if __name__ == '__main__':
    main()
