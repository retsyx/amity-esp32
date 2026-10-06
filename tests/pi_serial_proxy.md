# SSH over the Pi USB console

Use this bounded fallback when the Pi's Wi-Fi is occupied by the authorized
AP-only fixture. The Mac keeps its normal network. No Pi package installation
or network change is performed by the bridge. The Pi USB console must already
be logged in as `pi`; coordinate exclusive ownership of `/dev/cu.usbmodem31101`.
Do not use the ESP32 DUT's serial port.

`pi_serial_proxy.py` is an SSH `ProxyCommand`. It transfers its compressed
stdlib-only companion through the shell, saves the Pi terminal settings,
switches to raw/no-echo mode, and connects only to Pi localhost port 22.
Frames contain a one-byte type, a four-byte big-endian length, and up to
16 KiB of data. Explicit hello/close messages distinguish lifecycle events
from arbitrary SSH bytes. EOF or termination sends close; the Pi restores
terminal settings in `finally`. Bootstrap has a 10-second handshake timeout,
and a running connection has the configured lifetime (default 30 minutes).
Do not kill the bridge with SIGKILL; ordinary SSH control-master exit is clean.

From the repository root, with the installed IDF Python environment:

```sh
ssh -M -S test-results/pi-serial-control -fnNT \
  -L 127.0.0.1:2222:localhost:22 \
  -o BatchMode=yes -o StrictHostKeyChecking=yes \
  -o UserKnownHostsFile=test-results/pi-serial-known-hosts \
  -o 'ProxyCommand=/Users/yuval/.espressif/python_env/idf6.1_py3.14_env/bin/python /Users/yuval/projects-other/amity-esp32/tests/pi_serial_proxy.py --timeout 1800' \
  pi@pi-serial
```

The known-host file was populated from the physically attached Pi during the
initial bridge test. Adjust absolute paths for a different workstation.
Subsequent SSH/SCP calls use `pi@127.0.0.1` with port 2222. Once the DUT's AP
address is known, add its HTTPS forward through the existing master:

```sh
ssh -S test-results/pi-serial-control -O forward \
  -L 127.0.0.1:18443:DUT_AP_ADDRESS:443 pi@pi-serial
```

The CEC and BLE harnesses accept `--host pi@127.0.0.1 --ssh-port 2222`.
Use `--url https://127.0.0.1:18443` for their HTTPS calls. Keep authentication
logs and test output private under `test-results`.

Release the bridge and restore the serial shell before another USB user:

```sh
ssh -S test-results/pi-serial-control -O exit pi@pi-serial
```

Physical validation completed SSH key authentication and a read-only `uname`,
then transferred a known 1 MiB payload with matching SHA-256 in 0.367 seconds
(2789.8 KiB/s including SSH startup). USB CDC is not limited to a physical
115200-baud UART on this fixture. A subsequent executed Pi termios check
confirmed canonical input and echo were restored. OTA may still be limited
by the DUT's Wi-Fi and flash speed; its existing 300-second test timeout is
appropriate.
