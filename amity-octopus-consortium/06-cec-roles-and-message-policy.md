# Preserve the original local CEC behavior

**27 October 2026 user amendment.** The distributed layer delivers original intents to the selected owner. It does not compile a different CEC policy from persisted topology, capability or route approvals. This document owns local execution parity; [addressing](03-domain-model-and-addressing.md), [discovery](05-discovery-and-topology.md), [activity execution](07-activity-planning-and-execution.md) and [the authoritative amendment](21-original-controller-policy.md) apply.

## 1. Same local algorithm at every adapter

Each configured HDMI adapter runs the original front playback/back TV handling, including allocation, back-first scanning, front physical-address selection, identity announcements, received-frame handling and source isolation. Added Amity devices remain headless and act only for their authenticated original controller. They do not infer an independent activity or promote themselves after link loss.

The current activity is the source of truth for source isolation, with the
existing global standby activity as its special case. Compare its assigned
source by owning Amity, adapter and isolated CEC side, then by the ordinary
owner-local name/address lookup. Equal OSD names or logical addresses on
different wires do not identify the same source. A local source announcing
against a foreign assigned source receives directed Standby locally; its
announcement did not change the foreign wire, so no input repair is sent
there or substituted on this adapter. No separate active/inactive owner
flag, cached availability permission or cross-owner repair protocol is used.

Announcement ingestion precedes this response policy and never depends on
activity configuration. An early player may announce while a future activity
is still in flight and validly receive Standby under the current activity.
Its device record remains cached, and the normal activity sequence powers it
when that sequence reaches it. This accepted race does not justify a context
handoff workaround, pending-source immunity or additional coordination.

Saving activity configuration does not disconnect an Amity, discard native
HDMI device objects or remote adapter observations, or restart discovery. Device
identity, discovery progress and current wire state belong to the owner/adapter,
independently of saved activity references. Actual hardware, owner lifetime and
explicit discovery events retain their existing invalidation rules.
The only device-scan triggers are boot, an explicit UI request, and an actual
communication cache miss. Lookup scans have a two-minute cooldown independently
per owner/adapter/side; boot and UI scans bypass it. See [discovery](05-discovery-and-topology.md).

All device access uses the same cache-first owner-local resolver, including
source comparison and power/audio-status handlers. On a miss it applies the
same lookup cooldown and rechecks the same cache after an admitted scan. No
caller selects a cache-only variant or performs a cache-availability preflight.
Opaque lookup continuation preserves the original operation without blocking
the owner loop or requiring another received event. This follows Python
`ControllerImpl.get_device`; [discovery](05-discovery-and-topology.md#one-device-access-path)
owns the permanent encapsulation rule.

Moving equipment to another Amity must preserve every CEC frame, wire order, attempt budget, admission decision, repeat/release decision and existing timing. Transport and compatibility labels cannot select different algorithms. Preserve the baseline's existing reaction to received frames and discovery; do not replace it with a new mandatory verified-path policy.

## 2. Original activity operations

Use the original Hub decisions for selective power changes, same-target repair, standby, source input selection, explicit switch-input fallback and audio status. The owner obtains current LA/PA/type/vendor from ordinary local lookup and translates the original operation there.

The **29 October 2026 sequencing amendment** makes each Start/Standby sequence advance through the original Python recipient/command order, awaiting each lookup and actual transmission completion on that Amity. A later activity cannot overtake a paused sequence. Other Amities and the owner loop keep advancing. Key presses during activity setting are ignored before pressed-state registration; releases for those unregistered presses are no-ops. Add no ignored-press bookkeeping or replay. Preserve the existing startup/accepted-selection `wait_for_release` physical-input gate; after it clears, normal independent keys resume. Keys are never buffered behind an activity. The transient continuation and completion scope are defined in [activity execution](07-activity-planning-and-execution.md#2-reuse-the-original-decisions).

Power uses standard commands only: TV On sends Image View On (`04`), other
devices receive Power On Function (`44:6d`) and Released (`45`), and Off sends
Standby (`36`). Each owner derives these operations from its full activity;
the main does not send individual device power actions.
There are no configurable command recipes or vendor-dependent power branches.

A separate AVR/audio target is an ordinary qualified equipment selection. A source that cannot be found can still use the original explicit switch-device/input fallback. Missing one recipient does not authorize an all-or-nothing topology preflight that prevents the rest of the original algorithm from proceeding. Preserve baseline deduplication when one recipient fills several activity roles.

Do not require a stored receiver profile, capability approval, confirmed cable graph, proxy-resource reservation or physical-address evidence record before performing an operation supported by the original local algorithm. Do not construct a new opcode sequence merely because work is distributed.

## 3. Original input and gestures

Only the original controller accepts the sole remote and home/browser input. It keeps the original input gate, Power behavior, Siri macros, gesture recognition and generated press/release ordering. The main records only the owning Amity index for each raw key and delivers raw button edges to that owner. No adapter, side, OSD name or resolved recipient accompanies a key edge. Do not send periodic presses or per-button renewal messages.

One owner-level key table tracks each raw key. The receiver uses its current activity and shared resolver for every press, repeat and release; it does not retain a per-key CEC recipient. Each key continues independently after its own successful completion while it remains down. New keys and other CEC commands submit immediately; another key's outstanding ACK/completion does not block them. A failed key press ends that key; unchanged down reports or authority refresh cannot restart it. A matching later up is a no-op.

An accepted activity transition immediately ends prior holds and fences their pending user-key lookups and queued presses before the first wire edge. Already transmitting frames finish; old activity recipe frames retain their normal sequence. The existing input gate discards transition-period presses before registration and never replays them. Old releases and completions cannot affect a newer hold. This cutoff is local to each Amity and adds no cross-owner wait.

## 4. End-of-stream release and the only transmit FIFO

Resolve the remaining pressed keys against the current activity and aggregate them by physical wire, initiator and destination. `User Control Released` has no key operand. Ending one key must not release another active key at that recipient. Final up submits the recipient's release immediately behind admitted presses, without waiting for first edge, ACK or completion. Last-key failure may also end the stream. Preserve the required local completion/binding fences independently of button state, without retaining a per-key target.

The executor validates/translates and submits directly. CEC contains the only outgoing FIFO: eight queued-plus-active messages per wire, including releases. No extra release slot, repeat backlog or executor retry queue exists. Drop full-FIFO submissions, log every dropped TX including each member of a rejected atomic batch, and do not retain a dropped release for later capacity. State/completion trackers and bounded IPC are not outgoing frame buffers.

CEC mechanically queues and transmits exactly the frames and attempt budgets supplied. It must not inspect key opcodes to choose policy, repeat/release behavior, admission priority or retry budgets. Every key press has one wire attempt. Other budgets remain those selected by the original executor.

## 5. Transport fences

Authenticate the original controller and selected owner, reject stale boot/configuration/input work and preserve causal ordering. These are message-delivery protections. They must not create a second normal-operation planner, persistent activity availability or new CEC timing. Release uses the shared resolver under the current activity; activity and input lifetimes prevent old cleanup from affecting a later hold.

## 6. Validation

Compare baseline and scoped-owner transcripts for TV/source power, separate AVR audio, explicit input selection, source absence, received frames, same-target repair, independent held keys, finite taps, failed presses, FIFO saturation and releases. Host evidence must assert frames/order/attempts/admission, not only Ready or storage equality. Physical CEC captures remain necessary for timing qualification.

`tests/owned_source_theft_test.cpp` exercises the actual two-owner HubService
receive/admission path in both directions and direct Hub contexts with equal
source names/addresses across owners, adapters and sides. It checks local
directed Standby without a foreign input command or activity change, and
retains selected local-source acceptance, absent local-source handling,
empty-source switch fallback and the original global standby response.
Its three-owner regression configures all three TV/AVR/player activities before
cycling each owner through Start, both foreign-player announcements and Standby.
It checks power recipients, all six directed suppression responses, unchanged
selection/generation, selected-source acceptance and no foreign route repair.

The physical 0.1.99 three-owner run on 29 October proved nine typed discoveries
and passed the main-active cycle. During the member-2 cycle the harness completed
the main player's Active Source transmission, but observed no DUT response for
180 seconds. A successful broadcast transmission does not prove that the DUT
decoded it. That image lacked INFO evidence of receipt, applied owner context
and source-remediation admission; the missing-response cause remains unresolved.
See `test-results/device-removal-2026-10-29/consortium-three-duts.json` and its
wire log. Ordinary INFO traces now record those three boundaries, including
the current source and immediate Standby/input submission result. They add no
source-policy exceptions or coordination. Passing native receive-path tests
does not establish physical receipt or qualify this failed three-owner run.

`tests/owned_hub_test.cpp` checks that configuration edits retain native device
records and an in-progress scan, preserve selected and unselected remote adapter
snapshots, and submit no new discovery polls or identity queries.
