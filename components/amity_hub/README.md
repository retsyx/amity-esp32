`amity::Hub` contains the activity/device behavior independently of ESP-IDF.
It implements the front playback/back TV topology from `hdmi.py`, selective
power transitions, activity repair, source routing and switch-input fallback,
vendor power quirks, source-stealing suppression, device metadata discovery,
and per-bus power-status replies. Logical addresses remain scoped to their
own bus when resolving discovery updates.

`amity::HubService` owns two generic `cec::Bus` instances and serializes hub
state changes and received frames on its task. Each dispatched command submits
its frames to CEC without waiting for queue capacity, transmission or an ACK.
Public methods queue work and return whether that command was accepted; this
does not guarantee later CEC admission or acknowledgement. `snapshot()` returns
a copy; `on_change()`
runs outside the snapshot mutex, on the hub task. Keep callbacks brief and
schedule expensive application work on the appropriate subsystem task.
`configure_with_commit()` allocates its command and reserves capacity before
calling the caller's synchronous commit callback. Every command producer uses
the same counting semaphore, maintaining `queued + deferred + reserved <= 32`.
The worker returns a slot when the command is ready to execute, including after
any required metadata discovery. A full queue or a competing
configuration call returns before the callback runs. A failed callback returns
its error, releases the slot, and leaves the live hub untouched. A successful
callback is followed by a guaranteed nonblocking enqueue. Configuration calls
are serialized with a nonblocking mutex so commit order equals enqueue order.
Persistence remains entirely in the caller; the hub has no NVS dependency.
On dual-core targets a temporary core-1 initialization task allocates the CEC
GPIO timers there, keeping their high-rate interrupts off the default Wi-Fi
and BLE controller core. Startup still returns peripheral errors synchronously.
Single-core targets initialize directly; the generic CEC component does not
assume a core assignment.
Address allocation uses two unanswered polls and retries if a bus is busy.
Allocation and discovery advance through asynchronous poll completions and
bounded reply windows. Neither waits on the hub task for a CEC ACK, so received
frames, known-device commands and each held key continue to be handled.
The front tries playback 4, 8, 11, then CEC 2.0 backup addresses 12/13. The
back tries TV 0, then specific-use address 14, matching the Linux allocator.
At each boot the front physical address starts unknown. The owner claims the
back logical address and scans that bus first, including its bounded identity
reply window. It chooses the numerically lowest usable physical address reported
by a back device, or the configured default (`1.0.0.0` unless changed) if none
is reported. If the back logical address cannot be claimed, it still polls and
sends directed Give Physical Address from the unregistered address; those
replies are broadcasts. Only then does it claim and announce the front playback device and
scan the front bus. Both discovered and fallback results remain fixed until
reboot; later scans and unsolicited reports only update device metadata.
`Snapshot::front_physical_address` is `0xffff` until determination completes.

This inference follows the [CEC physical-address hierarchy](https://docs.kernel.org/userspace-api/media/cec/cec-ioc-adap-g-phys-addr.html):
an AVR at `2.0.0.0` precedes its children at `2.1.0.0`, `2.2.0.0`, etc.
Root `0.0.0.0`, invalid `F.F.F.F`, and malformed paths such as `2.0.1.0` are
excluded. A poll ACK without a valid physical-address report is insufficient.
Numeric minimum is not generally equivalent to the most trailing zero digits
across unrelated branches (`1.2.0.0` is less than `2.0.0.0`). This is a
best-effort estimate, not a replacement for EDID: if an AVR does not report its
address but a child does, the child's address may be selected instead.

Missing-device automatic rescans are
limited to once per two minutes; explicit discovery requests can rescan.
The owner checks the names needed by an activity/key command before applying
it. A command that needs missing metadata may be deferred until a permitted
rescan completes; this does not defer unrelated commands or held keys whose
recipients are already known. Deferred commands retain their command-capacity
reservation and are validated again when resumed.
Discovery builds a separate replacement list, preserving live power replies
and routing until metadata collection completes. An OSD name that moves to
a different logical address replaces its stale record; either metadata reply
order restores the selected source route once both name and physical address
are available.
`Snapshot::discovering` stays true until polls, queued identity requests, and
a 1.1-second quiet response window complete (bounded to five additional
seconds). Activity commands that need the missing metadata resume after this
window, so those names are resolved before display/source selection.

Identity replies mirror the Linux CEC core behavior inherited by the Python
application: OSD name `amity`, vendor ID zero, CEC version 2.0, physical
address/type, feature report with no advertised optional features, and
refused Abort. Directed unhandled opcodes are ignored as they are by the
Python follower. Power-state replies come from the selected activity: the
front reports on for any activity, while the back reports on only to its
selected source. Device-initiated source activation cannot change activity.

The held-button contract follows Python Amity's
[independent key state and keyed release](https://github.com/retsyx/amity/blob/main/main.py#L144-L208),
[per-key transmission coroutine](https://github.com/retsyx/amity/blob/main/main.py#L286-L320),
and [asynchronous CEC submission/completion](https://github.com/retsyx/amity/blob/main/cec.py#L669-L683).
Each key owns its pressed state and completion. The hub immediately attempts
to admit its first press to CEC's FIFO when it handles the command, even if
another key is awaiting an ACK. The completion of one press only controls that
key's next press.
`press_key(key, true)` permits at most one outstanding attempt for that held
key; a successful completion permits another while it remains pressed. There
is no repeat timer, frame backlog, or ACK wait on the hub task. New keys,
one-shot commands, replies, activity setup, and discovery can submit their own
frames while earlier transmissions remain outstanding. CEC still transmits
frames serially on each physical wire; the two wires progress independently.

`release_key(key)` ends only that key's hold. Frames already admitted to CEC
remain in FIFO order and finish, as in Python; completion cannot restart an
ended hold. A normal button-up does not advance a shared cancellation counter
or cancel another key's queued transmission. Every key-press frame has one
wire attempt. Admission failure or unsuccessful TX ends only that key's
pressed state and repetition, with one final-cleanup decision. Unchanged down
reports cannot restart a failed hold; a subsequent up and fresh down can.

Volume and mute target the activity's configured audio device; other keys
target its source. A held key retains its accepted bus, recipient and mapped
key for subsequent repeats and cleanup. The default play/pause control is
emulated once per press and keeps that mapping while held. `USER_CONTROL_RELEASED`
(`45`) has no key operand, so final release is scoped to the receiving
`(bus, logical address)` and is submitted only when all keys for that recipient
have ended, including a last-key failure. This extends Python's global
all-keys-ended/source-release behavior to release the actual audio or source
recipient correctly. One-shot keys, including Matter volume steps, send their
press/release pair unless another held key at that recipient still needs the
stream open.

For example, hold Volume Up, press Right, then release Right:

- Volume Up (`44 41`) continues independently throughout.
- Right (`44 04`) stops repeating when Right is released. If source and audio
  are different recipients, the source receives `45` and volume continues at
  the audio recipient, even when both devices use the same physical wire.
- If source and audio are the same recipient, Right-up sends no `45` while
  Volume Up remains held. Releasing Volume Up then submits that recipient's
  final `45`.

Outgoing frames are buffered only in CEC's FIFO: eight normal queued-plus-active
messages and one release-only overflow slot per wire. The executor retains
button state and bounded completion trackers, without a second frame queue.
Submissions never wait for capacity. Key presses use one wire attempt; final
releases and other ordinary commands use up to three. A final release may use
slot nine; at nine it is dropped and its cleanup decision is consumed. No tick,
later button-up, or newly available capacity retries that admission. This also
applies to power-quirk and switch-key cleanup. An admitted release follows the
recipient's already queued presses, and later commands cannot overtake it.

Activity changes, configuration replacement, standby, remote disconnect and
input loss cancel all held keys and clean up their recipients. If the hub
command queue itself is full, keyed release has a per-key emergency epoch;
explicit release-all has a global emergency epoch. These are overload
fallbacks, not the normal key-up mechanism. They invalidate the corresponding
older unexecuted key commands while allowing newer presses to proceed.

Remote input also follows Python's `wait_for_release` selection gate: startup
and successful selections block fresh forwarded buttons/swipes until every
physical button is released, with a one-second fallback. This includes an
explicit same-target activity selection; long-Power repair and rejected
requests do not arm it. `activity_selection()` notifies the input task even
when the activity generation stays unchanged. A gate cleared by the local
selection's final button-up is not armed again by its notification. Swallowed
holds cannot replay after timeout. See the [remote input contract](../ble_remote/README.md)
for release ordering, aliases and gesture handling. This is an input policy,
independent of the asynchronous CEC completion and settling state machines.

Speaker mute/volume state comes only from `REPORT_AUDIO_STATUS` from the current
audio device. It is unknown until reported and becomes unknown during an
adjustment; volume 127 also means unknown. Explicit mute/restore keys 0x65/0x66
route to audio. `GIVE_AUDIO_STATUS` is requested after activity selection and
coalesced after audio changes, with a 500 ms delay and no active audio hold.
That is a scheduling delay, not a guarantee of wire transmission or a reply
within 500 ms; bus traffic and admission can delay it.

Activity/configuration changes advance `Snapshot::activity_generation`.
`press_key_for_activity()` checks that generation on the owner task before
sending, so queued volume work cannot cross into a different activity, or a
later restart of the same activity index. `main/volume_sequence.hpp` paces
relative adjustments and cancels their remaining steps when this generation
changes, including old adjustments still waiting in the application queue.

The host tests exercise the behavior against captured outgoing messages,
including shared devices, power quirks, fallback switch input, source theft,
front/back identity, audio feedback, and same-address devices on two buses.
`tests/cec_hub_commit_test.cpp` covers independent outstanding held-key TX,
keyed releases and command admission without waiting for ACKs;
`tests/cec_bus_test.cpp` covers asynchronous FIFO admission, per-operation
completion, capacity limits and independent buses. `sh tests/cec_hub_test.sh`
and `sh tests/cec_bus_test.sh` run these host checks.

Activity admission uses `ActivitySettling` with the compile-time
`CONFIG_AMITY_ACTIVITY_SETTLE_SECONDS` (default 5; 0 disables it). Reservation
happens before enqueue, so rapid requests during setup are rejected rather than
queued for later. Same-target requests, including standby while already off,
are repairs and bypass the guard without starting another quiet period. Remote
Power uses the reserved target, so a second short press during startup cannot
be mistaken for a repair because the published snapshot has not caught up yet.
The web endpoint reports HTTP 409 with a settling message; Matter receives the
same admission failure, while remote requests are simply ignored.

Changed-activity setup submits frames without blocking and tracks their FIFO
completions asynchronously. Ordinary commands use up to three wire attempts;
key-press frames use one. The tracker includes known devices in both the
outgoing and incoming
activities, scopes addresses to each bus, and matches expected query replies
or the selected source's Active Source response to Set Stream Path. A matching
Feature Abort completes its request too. Setup completes only after its final
wire attempt; the quiet period then follows the last expected reply. Missing
replies have a two-second deadline before quiet time starts. Duplicates,
malformed replies, unrelated devices/opcodes and later repair/key traffic do
not restart it. Standby normally expects ACKs only, so its quiet period starts
at the final transmitted frame. Configuration replacement clears the old guard
because activity indices may change. `Snapshot::activity_settling` and
`/api/status.activity_settling` expose whether a transition is still settling.
