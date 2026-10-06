# Device settings

Loads and validates immutable provisioning records, then applies the separately
stored runtime hostname and front CEC physical address overrides. The host-testable
record parser accepts versions 1–3; version 3 adds the initial admin password hash. Matter consumes the loaded
commissioning values; the web server seeds its password only if none is saved.

Hostname changes write `amity_cfg/amity/hostname` and apply on reboot. They never
modify the factory record or activity backup document.

Front physical address changes write `amity_cfg/amity/front_physical` and apply
on reboot. The default is `1.0.0.0`; an empty input restores that default. Addresses
use four dotted hexadecimal digits and must form a valid CEC topology. This setting
is excluded from activity backup/restore and returns to its default on factory reset.

Factory reset writes a durable marker in `amity_cfg/amity_reset`, then reboots.
Before starting network or application tasks, the boot path snapshots any legacy
Matter commissioning blob into that journal, commits stage 2, erases normal NVS,
restores the legacy blob, and clears the `amity` application namespace. It removes
the stage marker last. The legacy snapshot is deliberately retained so power loss
during cleanup cannot destroy the original pairing code. No mutable task can
repopulate erased storage during the reset. Repeated stage 2 execution is safe.
The immutable factory partition, keys partition and OTA firmware slots are retained.

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
including partial namespace/partition erases, and verifies recovery for devices
with and without legacy credentials. The public reset API also requires a logged-in
session and an explicit boolean confirmation; the UI displays a confirmation prompt.
