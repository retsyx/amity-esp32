# Amity functional test harness

A Python functional test declares its DUTs, component topology and assertions.
The common framework configures the run, authenticates DUTs, owns the CEC
adapters and collects evidence. CEC I/O runs on the Raspberry Pi bench, locally
or over SSH. There is no boot-time virtual theater, device profile file or
permanently running follower service.

**Functional tests MUST use the existing authenticated HTTP APIs used by the
shipping Amity UI. Never add firmware APIs, test-only commands, authentication
bypasses, or test control surfaces for this framework.** The DUT helpers wrap
`/api/login`, the current activity editor/status/discover/configuration routes,
and the existing operation-status route. Additional harness helpers must wrap
existing UI operations as well. Firmware CEC, activity, input, commissioning and
membership behavior remains unchanged.

## Layout

- `TEST-HARNESS.md`: bench inventory and wiring; the only GPIO mapping source.
- `amity_harness/`: named DUT control, Markdown wiring parser, topology/address
  assignment, CEC transport and component emulation. Shared `config.py`,
  `testbed.py`, `runner.py` and `discovery.py` own runtime configuration,
  resource/authentication lifetime, CLI/reporting and scan evidence/assertions.
- `functional_tests/hdmi_scan.py`: the one functional test built on this framework.
- `unit_tests/`: hardware-free framework checks, separate from functional tests.

The former `tools/cec-harness/` fixed profiles, setup scripts and systemd service
templates have been removed. The static virtual-device inventory has also been
removed from `TEST-HARNESS.md`. The remaining wiring tables describe physical
connections, not simulated equipment.

## Writing a functional test

The single scenario is the complete 13-line `functional_tests/hdmi_scan.py`:

```python
"""One Amity discovers a TV, AVR and playback device."""

from amity_harness import DUT, TestBed, TvComponent, AvrComponent, PlaybackComponent


def test_hdmi_scan():
    dut = DUT("amity-dut")
    tv = TvComponent(dut, "Scan TV")
    avr = AvrComponent(dut, "Scan AVR", parent=tv, port=1)
    player = PlaybackComponent(dut, "Scan Player", parent=avr, port=1)

    with TestBed(dut, tv, avr, player):
        dut.scan_hdmi().assert_contains(tv, avr, player)
```

A functional test contains its equipment and behavior. Wiring parsing,
credentials, CLI options, resource cleanup, evidence helpers and CEC protocol
handling belong in the common framework. The TV, AVR and playback models keep
their existing `handle()` protocol behavior.

`TestBed` accepts DUTs and components in any order, and infers each component's
DUT. It supports several DUTs and configurations through the same interface;
this does not add another functional scenario. On entry it authenticates the
DUTs, checks their documented identities and opens their component session.
Exit attempts CEC cleanup and logout on success, assertion failure or exception.
`DUT.scan_hdmi()` uses the existing UI scan operations and returns a
`ScanResult`; `assert_contains()` checks the requested component identities and
fresh physical exchanges through the common discovery implementation.

## DUTs and wiring

`DUT("amity-dut")` implicitly reads `TEST-HARNESS.md`.
`Wiring.from_markdown()` finds the **HDMI-CEC harness GPIO mapping** table and
builds `(DUT name, tentacle, front/back) -> Pi GPIO`. It also reads DUT identities
and the setup/reset GPIO table. Duplicate assignments, unknown DUTs, missing
paired sides and overlapping pins are rejected before hardware access.

The runner supplies a scoped `RunConfig` for credential files, DUT URLs, TLS,
wiring, timeouts and CEC transport. The credential JSON contains `admin_password`
and must be private (mode `0600`). The default HTTP origin is
`https://<DUT name>.local`; use `--url` or per-DUT `--dut-url DUT=URL` to override
it. TLS verification is enabled by default: supply `--cafile` for a trusted
bench CA or explicitly choose `--insecure` for its self-signed certificate.
Use `--credentials PATH` for the run's default credentials, or repeat
`--dut-credentials DUT=PATH` for individual DUTs. `--wiring PATH` selects a
wiring document. These settings stay outside the functional test body.
Passwords and session tokens are never included in test reports.

Call `dut.press_fw_reset(3.5)` to request a physical 3.5-second hold; the
firmware interprets the gesture. FW RESET is active low. A hold drives only
its documented Pi GPIO low and releases the pin to input with pulls disabled
in `finally`, including on
interruption. A three-to-below-ten-second hold has the existing Add gesture
meaning; ten seconds has the existing factory-reset meaning. The HDMI scan
test does not press FW RESET, commission, rename, or change saved activities.

## Components and topology

Each `Component` attaches to a DUT, tentacle and CEC direction. It has a logical
and physical `CecAddress`, a name, a device type, and `PowerState.ON`,
`PowerState.STANDBY` or `PowerState.OFF`. Standby remains CEC responsive; Off
removes the logical address and suppresses CEC TX/RX, including ACKs. Power can
be changed explicitly with `set_power()`.

`TvComponent`, `AvrComponent` and `PlaybackComponent` provide role-specific
identity, discovery, power and control handling. They are configurable test
models, not implementations of every feature in the HDMI-CEC specification.
Components can transmit frames, process received frames, and use before/after
TX/RX hooks to observe, change or drop component messages for fault injection.
Hooks run synchronously during message delivery, including in the session
worker; keep them short and nonblocking. A hook or RX handler may transmit a
nested reply. Delivery is limited to 32 nested levels to reject echo loops,
without adding a message queue or delay. Driver-level ACKs and logical-address
claim traffic are outside these message hooks.

```python
from amity_harness import (
    DUT, AvrComponent, CecSession, PlaybackComponent, TvComponent, assign_addresses,
)

dut = DUT("amity-dut")
tv = TvComponent(dut, "Scan TV", direction="front")
avr = AvrComponent(dut, "Scan AVR", direction="back", parent=tv, port=1)
player = PlaybackComponent(dut, "Scan Player", direction="back", parent=avr, port=1)
components = [tv, avr, player]
assign_addresses(components)
# TV: 0 / 0.0.0.0; AVR: 5 / 1.0.0.0; player: 4 / 1.1.0.0.
with CecSession(components) as session:
    # Component RX/TX handling runs while DUT HTTP operations execute.
    session.assert_healthy()
    result = player.send(avr.address.logical, 0x44, [0x41])  # Volume Up
    assert result.transport == "virtual"  # No GPIO TX for this exchange.
    player.send(avr.address.logical, 0x45)  # Release
```

Address assignment follows parent/input-port topology for physical addresses and
component type for logical addresses. Logical addresses are independent on each
isolated CEC wire. Explicit addresses are accepted and validated. Optional
`reserved={gpio: {logical_addresses}}` avoids known existing occupants. Cycles,
duplicate addresses, invalid parent ports and excessive depth fail before I/O.

A `CecSession` treats each isolated GPIO as a separate virtual CEC bus.
Components on the same GPIO can exchange directed messages and replies without
sending those messages through the Linux adapter. A powered target returns a
successful virtual ACK (`status=1`); an Off target returns a virtual NACK
(`status=4`) and receives no frame or RX hooks. A component does not ACK its own
directed messages or polls. Polls report powered presence without calling an
RX handler. Virtual TX and RX use the same monitoring/fault hooks as other
component messages.

Broadcasts reach the powered sibling components on that virtual bus and are
also transmitted once on its physical wire for the DUT. Virtual delivery and
wire transmission are independent: a wire failure does not undo the virtual
delivery. The sender receives the actual wire result, and its `after_tx` hook
runs once. Different GPIOs remain isolated, including front/back sides,
adapters and DUTs; topology parent links assign physical addresses and do not
bridge their CEC buses.

TX/RX records label their transport as `virtual` or `wire`. Virtual records have
`wire_timestamp_ns=0` and never count as evidence of DUT wire traffic. The
session's virtual ACK represents component-model delivery, not an electrical
ACK from the Linux adapter. RX records preserve the original transport frame
and timestamp when a fault hook changes handling; `handled_frame` and
`handled_timestamp_ns` describe the modified frame separately.

The Linux backend finds `cec-gpio` adapters by stable `cec@<GPIO>` identity,
never `/dev/cecN` numbering. Components on a wire share one adapter and a worker.
It checks logical-address capacity, claims exactly the requested addresses,
uses passthrough mode for replies to the DUT and clears dynamic addresses when
the session closes. Linux chooses logical addresses by device type; unsupported
claims or collisions fail explicitly. Linux's restriction on electrical TX
between addresses claimed by one adapter applies only to the physical backend;
the session routes component-to-component messages on the virtual bus. No fixed
follower processes are needed. The bench's GPIO adapters support four logical
addresses, with CEC 2.0 restrictions checked by the driver; the scan uses two on
the back wire.

## Run the single HDMI scan test

Requires Linux/Pi, Python 3.10 or newer, the existing `cec-gpio` adapters and
permission to open `/dev/cec*` (normally membership in `video`). GPIO button
operations additionally require `pinctrl`. The framework has no third-party
Python dependencies. Stop/remove any previously installed `amity-cec@` follower
services before running it; they must not reclaim the adapters.

From the checkout root **on the Pi**:

```sh
python3 cec-harness/run.py \
  --credentials /private/path/amity-dut.json --insecure \
  --output test-results/functional/hdmi-scan-framework.json
```

The test uses only `amity-dut`, one TV on tentacle 0/front, and one AVR plus one
playback device on tentacle 0/back. It checks the DUT identity, submits the UI's
HDMI scan, and requires both fresh discovery exchanges on the CEC wires and the
correct names, types, logical addresses and physical addresses in the completed
UI scan. Cached names or a scan submission acknowledgment alone cannot pass.
Each component must answer fresh Give Physical Address and Give OSD Name
requests from the DUT on its actual wire. The initial test does not require
vendor metadata or qualify every optional CEC discovery feature. It derives
the DUT's initiator addresses from wire traffic, since the UI's legacy aggregate
address fields need not describe the owner-local CEC engine.
It closes the CEC session and logs out on success or failure. Other DUT wires
are untouched. The common runner writes private JSON reports under ignored
`test-results/`. Reports have top-level `test`, `passed` and `testbeds` fields;
each test bed contains its DUTs, components, scan evidence, CEC records and
cleanup outcome. A failed run also includes an error. Test functions need no
report-writing code.

Alternatively, keep HTTP authentication and credentials on the development
computer and run only CEC I/O over SSH on the Pi:

```sh
python3 cec-harness/run.py \
  --credentials /private/local/path/amity-dut.json --insecure \
  --harness-host pi@test2.local \
  --output test-results/functional/hdmi-scan-framework.json
```

Install this framework at `/home/pi/amity/esp32/cec-harness` on the Pi first, or
set `--harness-root`. `--ssh-port` supports an existing SSH tunnel. SSH uses the
normal known-host and authentication configuration. Its stdio worker transports
CEC frames and component identities only; it receives no DUT password or HTTP
session token. This is harness IPC, not an additional firmware API. Remote
kernel timestamps are mapped conservatively to the local monotonic clock so
queued pre-scan frames cannot count as fresh discovery evidence.

## Interactive use on test2

Use the Pi's ordinary Python 3; no virtual environment or third-party Python
packages are required. Start in the framework directory so its package is
importable:

```sh
cd /home/pi/amity/esp32/cec-harness
python3
```

At the prompt, authenticate interactively and keep a test bed open while you
inspect or control the DUT and components:

```python
from contextlib import ExitStack
from getpass import getpass
from amity_harness import DUT, TestBed, TvComponent, AvrComponent, PlaybackComponent

dut = DUT("amity-dut", insecure=True)
dut.login(getpass("Amity admin password: "))
tv = TvComponent(dut, "Test TV")
avr = AvrComponent(dut, "Test AVR", parent=tv, port=1)
player = PlaybackComponent(dut, "Test Player", parent=avr, port=1)

stack = ExitStack()
bed = stack.enter_context(TestBed(dut, tv, avr, player))
scan = dut.scan_hdmi()
_ = scan.assert_contains(tv, avr, player)
```

The password prompt does not echo or save the password. `insecure=True` accepts
this bench's self-signed HTTPS certificate. Wiring lookup and CEC setup are
automatic. When finished, run `stack.close()` to release the CEC adapters and
log out. Scripted tests use `with TestBed(...)` for automatic cleanup.

Host-only checks from the checkout root:

```sh
PYTHONPATH=cec-harness python3 -m unittest discover -s cec-harness/unit_tests -v
```

These validate the framework, not DUT behavior.

## Validation — 28 October 2026

The single HDMI scan scenario passed on `amity-dut` firmware `0.1.84` with CEC
transport on `test2.local`. The UI reported all three fresh component identities
with their expected types, logical addresses and physical addresses. Captures
confirmed Give Physical Address/Report Physical Address and Give OSD Name/Set
OSD Name exchanges with the DUT on GPIOs 23 and 24. DUT authentication stayed on
the development computer. Private evidence is in
`test-results/functional/hdmi-scan.json` at the repository root.

After the run, all 14 bench CEC adapters had an empty logical-address mask. The
old follower services, profiles and installed setup command were absent. This
qualifies the initial scan and dynamic fixture lifecycle; it does not claim
AV picture/audio, remote activity parity, all optional CEC features, or physical
FW RESET gesture coverage.

The framework's host-only unit checks and Python compilation checks passed.
No production firmware code or API was changed for this framework.

After adding virtual component routing, the same physical scan passed again;
evidence is in `test-results/functional/hdmi-scan-virtual-routing.json`. Its
records distinguish the actual DUT wire exchanges from virtual peer delivery.
Host checks cover directed virtual requests/replies, polls, Off behavior,
fault hooks, bus isolation, broadcast wire failures and bounded response loops.

The 13-line scenario using the common runner and `TestBed` also passed on the
physical harness. Evidence is in `test-results/functional/hdmi-scan-framework.json`.
All 97 framework unit checks passed locally and on the Pi. Common cleanup
released all 14 CEC adapters after the scan; DUT credentials stayed local.
