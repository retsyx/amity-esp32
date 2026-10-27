# Device settings

Loads and validates immutable provisioning records, then applies the separately
stored runtime hostname and per-adapter front CEC physical-address defaults. The host-testable
record parser accepts only version 3, including the optional initial admin password hash. Matter consumes the loaded
commissioning values; the web server seeds its password only if none is saved.

Hostname changes write `amity_cfg/amity/hostname` and apply on reboot. They never
modify the factory record or activity backup document.

Per-device **Amity → Settings** saves a complete four-slot preference record in
`amity_cfg/amity/front_defaults`, with a durable revision and an atomic batch.
The old `front_physical` scalar is read only when that record is absent and
migrates into slot0 alone. Other slots start at `1.0.0.0`. Stable internal slots
0–3 are displayed as HDMI adapters 1–4 within the hardware profile's capacity;
absent, unconfigured and unknown-presence slots retain their own settings.
The reader accepts prior longer preference records only after validating every
stored address, retains the four supported entries and their durable revision,
and writes only four entries on the next save. It rejects malformed trailing
addresses or extra data; it does not expose an additional adapter slot.

The stored copy and boot-applied copy remain separate until restart. Empty input
resets that slot to `1.0.0.0`; the existing dotted hexadecimal topology validator
rejects holes and `F.F.F.F`. Defaults are excluded from activity backup/restore
and reset with the application namespace. A usable address from applicable
native evidence takes priority over that same front's default. A default is an
unconfirmed fallback hint, never an observed address, route confirmation or
permission to replay an activity. Legacy adapter0 retains its existing startup
fallback behavior; typed execution still requires qualified topology evidence.

The shared settings service checks the target BootId, durable settings revision,
original authority deadline and native attachment generation before persistence
on the owner task. Writes require positive presence: fixed legacy hardware is
present by construction; detachable hardware needs a separate configured GPIO
detector, and CEC silence is never a presence detector. The target must be idle
and quiescent. The durable applied result's transmission is attempted before
scheduling that target's restart, even if the connection has already closed.
A lost reply leaves the client uncertain; it does not cancel the explicitly
requested restart or repeat the write. Other devices are not restarted. The dialog reports saved, observed and
selected addresses separately and performs no scan on read. Software tests do
not qualify physical four-adapter wiring or presence sensing.

Factory reset writes a durable marker in `amity_cfg/amity_reset`, then reboots.
Before starting network or application tasks, the boot path erases normal NVS
and clears the `amity` application namespace. It removes the pending marker last,
so power loss safely repeats all erases. No mutable task can repopulate erased
storage during the reset. Commissioning credentials remain in the untouched
factory partition, preserving the original setup PIN/QR credentials.
The immutable factory partition, keys partition and OTA firmware slots are retained.

Explicit removal of a responding added device reuses this same journal through
`system_services/member_removal.cpp`. The authenticated runtime reserves a
short-lived exact removal permit, retires and drains the native owner, and the
service rechecks the permit on the internal flash broker immediately before
committing the marker. No raw NVS operation runs on the external TLS stack.
Only after the durable result transmission is attempted does the selected
device schedule restart, including when that reply cannot be delivered. The
main, generic membership retirement, enrollment abort, lost authority and
unreachable devices cannot invoke this reset callback. A management-only
committed member needs its authenticated membership proof, not an executable
route graph. Offline removal only drops the main's roster entry and reports
that physical reset is required. No new erasure loop or reset journal is added.

The optional FW_RESET GPIO adapter starts after settings initialization and before
network services. Both GPIO edges notify a worker for an active-low input with a
pull-up. The worker debounces for 50 ms using a notification timeout,
and schedules the same durable reset after a ten-second hold. It restarts only
after the journal commit succeeds; on failure it logs the error and requires a
release before retry. A debounced release after boot is required to arm it, so a
stuck or still-held button cannot repeatedly erase storage. Both the button and
web reset requests share an atomic scheduling guard. `CONFIG_AMITY_FW_RESET_BUTTON`
defaults to enabled. On current devices without the circuitry, the internal
pull-up keeps unused GPIO4 (XIAO D3) inactive. Disabling the option omits GPIO configuration
and the worker task, for boards using the pin for other hardware. The worker
blocks indefinitely when idle and wakes only on an edge or debounce/hold deadline;
it retains a 3 KiB stack to perform NVS writes and restart outside the ISR.
`tests/reset_button_test.cpp` exercises the GPIO-independent button policy.
`tests/reset_button_interrupt_test.cpp` drives the GPIO ISR and worker through
virtual edges and timeouts, including an edge arriving immediately before the
worker blocks, boot holds, cancellation, failed-reset retry and startup cleanup.

With button hardware available, verify GPIO4 is high when released and low when
pressed, short presses preserve settings, and a released-then-held ten-second
press restores factory settings while retaining the original commissioning code.
Keep it held through restart to verify no second reset occurs. These electrical
and end-to-end checks have not been performed on the current buttonless devices.

`tests/factory_reset_test.cpp` injects power loss at each persistence operation,
including partial namespace/partition erases, and verifies recovery while retaining
factory data, NVS keys and both firmware slots. The public reset API also requires a logged-in
session and an explicit boolean confirmation; the UI displays a confirmation prompt.
