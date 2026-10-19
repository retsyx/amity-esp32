`amity::Hub` contains the activity/device behavior independently of ESP-IDF.
It implements the front playback/back TV topology from `hdmi.py`, selective
power transitions, activity repair, source routing and switch-input fallback,
vendor power quirks, source-stealing suppression, device metadata discovery,
and per-bus power-status replies. Logical addresses remain scoped to their
own bus when resolving discovery updates.

`amity::HubService` owns two generic `cec::Bus` instances and serializes hub
commands and received frames on its task. Public methods queue work and
return whether it was accepted. `snapshot()` returns a copy; `on_change()`
runs outside the snapshot mutex, on the hub task. Keep callbacks brief and
schedule expensive application work on the appropriate subsystem task.
`configure_with_commit()` allocates its command and reserves capacity before
calling the caller's synchronous commit callback. Every command producer uses
the same counting semaphore, maintaining `queued + reserved <= 32`; the
worker returns a slot immediately on dequeue. A full queue or a competing
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
it, performs any permitted rescan, then executes that original command.
Discovery builds a separate replacement list, preserving live power replies
and routing until metadata collection completes. An OSD name that moves to
a different logical address replaces its stale record; either metadata reply
order restores the selected source route once both name and physical address
are available.
`Snapshot::discovering` stays true until polls, queued identity requests, and
a 1.1-second quiet response window complete (bounded to five additional
seconds). Queued activity changes run after this window, so metadata names
are resolved before display/source selection.

Identity replies mirror the Linux CEC core behavior inherited by the Python
application: OSD name `amity`, vendor ID zero, CEC version 2.0, physical
address/type, feature report with no advertised optional features, and
refused Abort. Directed unhandled opcodes are ignored as they are by the
Python follower. Power-state replies come from the selected activity: the
front reports on for any activity, while the back reports on only to its
selected source. Device-initiated source activation cannot change activity.

Volume and mute keys target the activity's audio device; other keys target
its source. `press_key(key, true)` starts a held stream. The owner waits for each
queued CEC press to finish, handles pending commands, and immediately submits
the next press if the button is still held. There is no repeat timer or backlog
of queued repeats. Each held attempt uses the normal CEC arbitration and idle
timing; failed attempts can repeat only while still held. Release cancels
continuation through an atomic generation immediately, even while the owner is
waiting for transmission or discovery. A frame already in progress finishes,
then the release is queued; a quick tap still sends its initial press/release.
Activity changes, configuration replacement and standby also cancel the held
stream. One-shot keys (including Matter volume steps) keep their existing
press/release behavior. Held keys release the device they were sent to. The default
play/pause key is emulated once per press and retains that mapping while
held. Speaker mute/volume state comes only from `REPORT_AUDIO_STATUS` from
the current audio device. It is unknown until reported and becomes unknown
again while an adjustment is pending. Volume 127 means unknown; no absolute
volume is invented when a device provides none. Explicit mute/restore keys
0x65/0x66 are routed to audio, and `GIVE_AUDIO_STATUS` is requested after
audio changes and activity selection.
Audio-change queries are coalesced until input has been quiet for 500 ms,
so a long volume adjustment does not overwhelm the low-bandwidth CEC bus.
The transport waits up to one second for TX capacity. A rejected initial key
press does not change held-key/playback state; a rejected key release remains
pending and is retried by the owner tick, including releases from power quirks
and switch selection. A new key cannot overtake a pending release to its
recipient. These guarantees concern queue admission; physical transmission
still uses the CEC driver's bounded retries and error counters.
If even the hub's command queue is full, `release_key()` accepts an emergency
release through an atomic generation counter. The owner releases its held key
and skips older queued key events, including repetitions accumulated during
discovery. Keys admitted after that release retain the new generation and
are processed normally; command-capacity reservations remain unchanged.
Activity/configuration changes advance `Snapshot::activity_generation`.
`press_key_for_activity()` checks that generation on the owner task before
sending, so queued volume work cannot cross into a different activity, or a
later restart of the same activity index. `main/volume_sequence.hpp` paces
relative adjustments and cancels their remaining steps when this generation
changes, including old adjustments still waiting in the application queue.

The host tests exercise the behavior against captured outgoing messages,
including shared devices, power quirks, fallback switch input, source theft,
front/back identity, audio feedback, and same-address devices on two buses.

Activity admission uses `ActivitySettling` with the compile-time
`CONFIG_AMITY_ACTIVITY_SETTLE_SECONDS` (default 5; 0 disables it). Reservation
happens before enqueue, so rapid requests during setup are rejected rather than
queued for later. Same-target requests, including standby while already off,
are repairs and bypass the guard without starting another quiet period. Remote
Power uses the reserved target, so a second short press during startup cannot
be mistaken for a repair because the published snapshot has not caught up yet.
The web endpoint reports HTTP 409 with a settling message; Matter receives the
same admission failure, while remote requests are simply ignored.

Changed-activity setup uses FIFO completion waits with the ordinary three wire
attempts. The tracker includes known devices in both the outgoing and incoming
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
