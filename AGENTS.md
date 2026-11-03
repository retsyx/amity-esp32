# Preserve original Amity behavior

## General

This project is in early development. There is no need for backward compatibility with prior revisions of the software. This remains true as long as this instruction is in this file. When the software is more mature, this instruction will be replaced with an instruction to ensure backward compatibility between software revisions.

## Adapter connections — 1 November 2026

There are no separate inputs that detect whether an HDMI adapter is attached.
Do not introduce presence GPIOs, detector states or attachment generations.
Configured CEC wires remain usable; actual CEC device responses and command
ACKs determine communication results. An absent adapter produces no CEC
responses. Do not add another availability gate based on a sensor, saved
selection, cache observation or an assumed physical topology. Keep the original
resolver, scan triggers, electrical CEC handling and attempt budgets.

Adapter settings use the configured slot, current owner boot, settings revision
and authority, followed by atomic save and live application on the selected
owner without reboot. They do not require a responding CEC device or physical
attachment proof. Preserve actual binding, activity, boot, authority, deadline
and transport fences.

## Live adapter address settings — 2 November 2026

Changing an adapter's default physical address must not reboot an Amity. Store
the latest usable scanned address, or no address if the most recent applicable
scan found none. On an explicit atomic default save, select that stored scanned
address when present, otherwise the new default. Normal scans update the stored
observation while leaving the selected address unchanged; the next default edit
uses the latest scan result, including when a scan finds or loses an address.
Do not retain a separate flag recording whether startup used the default.
Preserve independent adapter settings, device caches, authenticated connections
and the original scan triggers. No settings-triggered enumeration or activity
replay is added.

The settings dialog uses one table with **Adapter**, **Default**, **Detected
Address** and **Selected Address** columns. Adapter numbers are 1–4 within the
hardware capacity; Default contains the editable text field. Use **Save** and
remove settings-restart wording, repeated front-address labels and the text
“Configured HDMI adapters can be edited.”

## TLS connection reuse — 1 November 2026

TLS connection establishment is independent of application synchronization.
An exchange uses the existing connection to its peer; a missing connection is
created implicitly. Reuse a healthy connection, including while synchronization
is incomplete. Never close it to replace it with another connection merely
because synchronization failed. Keep authentication, membership, boot, replay
and authority fences, and withhold operational readiness until synchronization
succeeds. Recover application exchanges on the same authenticated stream;
actual transport loss or an unusable stream still requires reconnection.

## Control and activity invariants — 1 November 2026

- One authenticated, ordered TCP connection per Amity pair. No renewable input/control leases, generic replay IDs, or random key-hold tokens.
- Boot and activity lifetimes reject stale work. Keep authentication, configuration checks and operation deadlines. Administrative operation IDs may prevent duplicate actions after retries.
- Amities agree on activity start/stop intent. Each independently runs its own ordered device sequence. No cross-Amity CEC waits, step planning, or device synchronization; one owner's blocked job must not delay another owner's activity delivery or execution.
- Within each Amity, each action waits for its lookup and CEC transmission before the next action. Send old-only devices' Standby before new-only devices' power-on; preserve shared-device behavior. Later activity requests cannot overtake that local sequence.
- Never wait for reported device state, application replies, or a quiet period to advance an activity.
- The resolver may have multiple pending lookups, but resumes waiters FIFO. Each caller stays paused; cached independent work and the owner task remain responsive.
- The key API sends a complete press/release: duration 0 is a tap; 1–5000 ms is a bounded hold. Cleanup does not depend on the caller remaining connected.
- The main sends only key-down/key-up state changes and remembers the receiving Amity index. Each owner uses the full current activity and the shared resolver for key actions, including release; do not retain a per-key resolved CEC recipient. Held-button CEC press transmissions are generated locally by the owner.
- When an Amity accepts an activity transition, stop old holds and invalidate old pending user-key lookups and queued user presses before initialization. No further old user press may start; an already-transmitting frame can finish. Old releases/completions cannot affect new holds. Normal key-up still preserves admitted-frame FIFO behavior.

These supersede older execution-lease, generic replay, response-settling and per-key recipient-retention requirements below. Human login sessions and administrative retry IDs are separate.

## Explicit Amity removal — 29 October 2026

An explicit Remove deletes the Amity from saved configuration. Remove its owner
record, saved name, adapter settings and equipment, and delete every activity
that references that equipment through source, display, audio or switch. Show
the affected activity names and warn that confirming removal deletes those
activities. Confirm against reviewed configuration/presentation and membership
revisions, target boot and exact affected activity IDs. A changed review needs
fresh confirmation. Successful completion leaves no saved owner/equipment
selector or ordinary metadata for the removed Amity. Existing removed records
can be deleted too. Preserve unrelated data and slot metadata; ordinary deleted
activity slot reservations contain no device reference or retained alias.

Replace remains supported separately. It preserves and explicitly remaps the
selected equipment and activity definitions to the replacement, revokes the
isolated old device and removes its saved record. Do not require destructive
Remove before Replace. Omit the device-card controller/capacity sentence and
the replacement dialog's remote-ownership sentence.

Missing/offline devices retain saved selections until explicit Remove or
Replace. Keep reset acknowledgment, authority, transaction and stale-work
fences. Transient reset/replay journals do not authorize retained normal device
records or a successful response before configuration cleanup finishes.

## Lessons — 29 October 2026

I should have followed the original Amity implementation earlier, instead of adding coordination and special cases around valid behavior.

The concrete lessons are:

- **Current means current.** An activity in flight is not active. Early Standby is valid; normal activation later powers the device on.
- **Discovery is independent of configuration.** Announcements populate the device cache. Editing activities must neither clear it nor disconnect member links.
- **Device lookup has one path.** Every caller uses `find_device()`. Only that function knows about caching and the two-minute scan cooldown.
- **Scans have exactly three triggers:** boot, an explicit UI request, or a required-device cache miss.
- **Activity steps are sequential per Amity.** Different Amities may progress concurrently. User presses during setup are discarded through the existing input gate.
- **Distribution delivers ownership and messages.** It should not introduce another activity planner or source policy.
- **Tests should expose problems, not conceal them.** Verify fresh physical commands and resulting state without adding workarounds for legitimate timing differences.

The broader lesson is to identify the actual invariant before “fixing” a race. Not every possible ordering is a bug.

## Automatic Matter setup at boot — 28 October 2026

Matter commissioning must open automatically at boot when the current Matter
fabric count is zero and the board is either unclaimed or the proven original
main with retained authority. This is a permanent user requirement. Do not
remove it, hide it behind a build option, or replace it with a button press
without a new explicit user request. No FW RESET GPIO is required. Unclaimed
reboots and factory-reset boots open the bounded setup window again.

Use current fabric count, not a has-ever-been-commissioned latch or the presence
of saved activities/configuration. A proven original main whose last fabric was
removed also opens setup at its next boot while keeping the same immutable main
role, UI and authority. A board with a current Matter fabric must not open setup
merely because it reboots.

Resolve stored identity and role before the decision. Amity members are already
enrolled and remain headless even without a Matter fabric; prepared, retired
and recovery roles must not automatically open Matter setup. Use the existing
shared host and setup arbiter. Preserve optional manual reopening, the
three-to-below-ten-second Amity Add gesture and the ten-second factory-reset
gesture. Regression evidence must exercise the actual boot path, not merely an
unused eligibility helper.

Setup cards retain only the original provisioner's native Amity/Matter QR,
manual code, hostname and Initial password image, plus the user-authorized USB
MAC and bench labels. Do not add setup instructions or change credentials/QR
images when updating cards.

## Adding an Amity amendment — 27 October 2026

The accepted workflow is **Amity devices → Add** on the original main, which
scans directly for nearby unclaimed boards. The user holds and releases the
candidate's FW RESET button after at least three and before ten seconds,
chooses the candidate if several are found, enters its name, and adds it.
Keep manual Matter reopening and the ten-second factory reset rule; boot
Matter setup opens automatically as required above.
A committed controller/member cannot become an unclaimed candidate by rebooting.

The default admission profile uses fresh ephemeral key exchange and AEAD under
that physical window. `CONFIG_AMITY_ADMISSION_REQUIRE_SETUP_CODE`, default off,
selects the stricter profile: authenticate with the candidate's existing printed
Matter manual/QR setup code using PAKE to authenticate fresh Amity-specific
session keys.
Both ends must require the same profile and reject a mismatch without downgrade
or fallback. This is not full Matter commissioning, does not create a fabric,
and cannot change role through the Matter stack. Operational admission still
commits the headless member role through the existing membership transaction.

Do not introduce or retain a separate Add secret, Add-code provisioning/rotation
workflow or stored bootstrap verifier. Preserve stable origin IDs and the
independent factory Matter/admin credentials. Discovery is direct from the main;
there is no admission relay through another member. Do not put internal protocol
or build-profile terminology into the ordinary user flow.

## Qualified lookup amendment — 27 October 2026

The user explicitly requires the persistent equipment selector to be **Amity
device ID / HDMI adapter / CEC side / OSD name**. The coordinator looks up the
owning Amity; that owner looks up its adapter, isolated CEC side and current
device by OSD name. Run the original local Amity algorithms as close as possible
to the equipment. Distribution is a dumb ownership and message-delivery layer,
not an alternative activity planner or CEC policy.

Do not persist an activity `enabled` flag, executable route approval, confirmed
topology, resource graph, capability approval, or physical-address evidence as
permission to use an activity. Save the user's original activity fields,
equipment selectors, names, ordering and shortcuts. A missing owner,
adapter or OSD name remains a saved selection; normal runtime lookup and the
original local handling determine the result. A device returning or changing
its CEC logical address must not require re-enabling, rebuilding or approving
the saved activity. Preserve AVR, separate audio and explicit switch-input
behavior, including the original non-CEC source fallback.

This does not remove necessary device identity, membership/authentication,
original-controller authority, configuration/storage/reset journals or adapter
settings. Keep the runtime discovery, active-input, transmission-completion and
exact-recipient key cleanup state needed by the original algorithms, and the
transport fences needed to reject stale or unauthorized messages. Do not turn
those mechanisms into persistent normal-operation approval gates, durable
activity-resume journals, new discovery timing, or a second outgoing queue.

### Lesson from the failed activity migration

Coordinator-side cached equipment observations are for UI/diagnostics, never an
additional availability permission. Power and key work must reach owner-local
lookup even when the coordinator has no report. If an original source-input
lookup determines a later switch fallback, use its actual owner-local result;
do not substitute cache presence, age or a profile/approval flag. Bounded typed
request/result correlation is transient communication state, not a CEC frame
queue or a persistent plan.

Preserving activity JSON fields is not preserving activity behavior. The former
migration retained six activities but made AVR/switch activities permanently
non-executable through an added `enabled`/route-approval model. Before declaring
a migration or distributed adaptation successful, compare its execution with
the relevant baseline source and tests, including separate AVR/audio, explicit
switch inputs, absent equipment and its later return. Verify local and remote
frames, ordering, attempts, completion-paced activity settling/input readiness
and key lifecycle; counts, labels, storage equality,
a successful build or a Ready endpoint alone do not establish parity.

Do not tell the user to repair, reconfigure or approve existing activities to
work around an architectural regression introduced by the distributed layer.
Identify and remove the extra policy, update the owning design/tests, and report
the actual behavioral limitation while it remains unresolved. Older design
documents and tests cannot authorize contradicting the user's simpler model.

For held buttons, each key has its own completion-paced continuation at the
emitting executor: a successful attempt permits that key's next press while
it remains down. New keys and other CEC commands submit immediately when
received by the executor, without waiting for another key's ACK, completion,
or queue capacity. The owner task must keep advancing input, received frames,
activity setup and discovery while transmissions are outstanding. Communicate
button state changes between Amity devices, not periodic presses or per-button
renewals.

The following are explicit user changes to the baseline, dated 21 October 2026:

- Buttons remain active independently. Releasing Right while Volume Up is held
  ends only Right. Normal button-up neither advances a shared cancellation
  counter nor cancels already admitted CEC frames; those finish in FIFO order.
- Preserve Python Amity's `wait_for_release` input gate at startup and accepted
  activity selections, including same-target selection, with all-buttons-up
  clearing and the one-second fallback. Swallowed holds must not replay after
  expiry. Rejected selections and long-Power repair do not arm the gate.
- Support Python's fixed Power+Select/Up/Right/Down/Left activity macros only
  for Siri remotes: activities 0–4 for short selector holds and 5–9 at 500 ms.
  Execute once on either macro button's first release, measure duration from
  the selector press, and consume the macro until all physical buttons are up.
  These defaults do not require configuration.
- Every CEC key-press frame uses one wire attempt; remove key-press retries.
- Any failed key-press transmission ends that key's pressed state and stops its
  repetition. A later matching button-up is a no-op. Unchanged down reports and
  authority renewal must not restart it.
- CEC release is implicit at the end of the executor's press stream, after one
  or more presses, when no keys remain pressed for that receiving CEC device.
  Remote button-up updates state; it is not a direct key-specific release TX.
  Release has no key operand. Aggregate keys per physical wire, initiator and
  destination; ending one key must not release another active key there.
  Final key-up submits release immediately behind admitted presses, without
  waiting for ACK, completion or first edge. A last-key TX failure may likewise
  cause a final release attempt. Retain
  exact safe stream cleanup state separately from the ended key's token.
- Do not buffer outgoing messages in the executor. The CEC layer contains the
  only transmit FIFO, with eight slots per CEC wire counting queued and active
  messages together. Releases have no extra slot: full-FIFO submissions are
  dropped, and every TX dropped because the FIFO is full must be logged.
  The executor validates/translates and submits directly; admission failure is
  reported without a second frame queue or a hidden repeat backlog.
- CEC is a mechanical transmitter. It queues and sends exactly the frames and
  attempt budgets supplied, performs wire protocol/timing, and reports results.
  It does not interpret key opcodes to choose policy or decide button state,
  repeat, release, or which message to send. Those decisions belong to the
  executor. State and completion tracking are not an outgoing message buffer.

The user clarified that queues remain appropriate where necessary for
communication between threads/tasks. This permits bounded IPC handoff of input
state or typed requests; it does not permit an executor backlog of outgoing
messages, retained ordinary work waiting for CEC capacity, or retry queues.
Keep the existing input/gesture algorithms and task ownership unless a change is
needed for an explicitly requested behavior. Preserve causal down/up ordering
and failed-key no-op handling under IPC pressure. The CEC FIFO has eight slots
for all messages, including releases. If full, drop the submitted release
without retaining or retrying it when capacity opens. Log every TX dropped
because of full-FIFO admission, including every member of a rejected atomic
batch. CEC must not inspect opcodes to choose admission policy.

This preservation rule explicitly includes Siri remote swipes. Keep the
original gesture recognition, thresholds, direction and acceleration rules,
timing, generated press/release sequences, cancellation and activity-generation
handling. Transport the resulting original events to their owner without
reinterpreting the gesture, changing a tap into a hold, or adding a new swipe
algorithm.

Explicit user amendments remain authoritative, including the sole original
controller and remote, headless added devices, no automatic failover, detachable
HDMI adapters, and the approved Amity devices tab and per-device settings.
See `amity-octopus-consortium/21-original-controller-policy.md` and
`amity-octopus-consortium/20-user-interface.md` for those requirements. Internal
development terms must not appear in user-facing UI or documentation.

Before changing an existing algorithm, compare the relevant baseline source and
tests. Verify behavior parity for unaffected local operation and the equivalent
distributed path. Record necessary distributed adaptations and evidence in the
owning design document; a conflicting older design proposal or test does not
authorize changing the original behavior.

## Current saved format during development — 29 October 2026

Support only the current saved configuration format. Remove prior-format
decoders, conversion paths, migration backups/maps and compatibility aliases.
Use versionless project types, functions and HTTP route names. Keep numeric
format/version validation, explicit unsupported-version errors and the storage
transaction framework for future deliberate changes; no format migration runs
while the codebase is still being developed before its first release.

Blank storage creates an empty current-format configuration directly using the
proven original controller and current hardware capacity. Unsupported or corrupt
committed records must not be silently replaced. Preserve current valid records,
identity/authority, credentials, Matter setup, original CEC behavior and journal
integrity. Numeric current-protocol metadata and external SDK standard names
retain their actual meaning.

## Activity edits and live discovery — 29 October 2026

Saving or applying activity configuration must not disconnect Amity devices or
clear their HDMI discovery caches. Keep authenticated member connections and
observed equipment independent of saved activity selections, including adapters
with no selected equipment. Preserve the exact revision, boot, link, authority,
lease and replay checks that reject stale work while updating configuration on
the existing connection. The current activity is the sole source of truth for
source-theft handling, with the special standby activity when the whole system
is in standby; do not add an active/inactive owner flag.

Valid CEC device announcements populate or update the device cache regardless
of activity configuration, including standby and devices absent from every
saved activity. Learn the announcing address and reported information and query
that peer for missing identity metadata without requiring a full bus scan.
Activity policy may respond to the announcement, but must not determine whether
the device is cached or erase its observation.

An activity still in flight is not an active activity. A device announcing
before its Amity receives/applies the start may validly be put in standby under
the current activity; the normal start sequence supplies the power commands.
Do not add a pending-source exception, announcement/configuration coordination
or a test guarantee that prevents this valid timing outcome.

## HDMI scan triggers — 29 October 2026

HDMI device enumeration is permitted for exactly three reasons: board boot,
an explicit scan requested by the user in the UI, or an actual communication
attempt whose target device is missing from the cache. Do not add periodic,
reconnection, configuration-change or activity-change scan triggers. Keep
passive received-device observations and the adapter's own CEC logical-address
maintenance separate from device enumeration.

Cache-miss lookup scans have a 120-second cooldown independently for every
Amity / tentacle / CEC side. Scan only the missing recipient's side and preserve
the opposite side's cache. Repeated misses during cooldown remain unresolved;
do not defer or queue another scan for later. Boot and explicit UI scans bypass
that lookup cooldown and refresh the scanned sides' cooldown timestamps. A
real communication miss during activity cleanup still
qualifies; editing configuration alone does not. Retain HDMI caches through
activity edits and connection/configuration updates.

All device access goes through one resolver. That function
checks the cache, performs a throttled scan on a miss, then checks the same cache
again. The resolver owns the cache and all lookup/scan/cooldown decisions.
Callers must not know whether a result was cached, inspect cache availability,
preflight whether discovery is needed, or request a lookup scan themselves.
Do not introduce passive-handler, cache-only or cache-bypassing lookup paths.
Boot and explicit UI scan requests may force enumeration; ordinary device
access must always use the shared resolver.

The ESP32 owner task must stay responsive while resolution waits for scanning.
Resume the original operation after the resolver completes, rather than require
a second input event or duplicate already-emitted commands. Keep asynchronous
lookup continuation inside the shared mechanism and existing bounded typed IPC;
do not add an outgoing CEC queue. Preserve exact recipient and activity/boot
fences when resuming work. Tests must exercise the actual callers with a cold
cache, successful scan, repeated miss/cooldown, and independent tentacle sides.

## Sequential activity execution — 29 October 2026

The 1 November control/activity invariants above govern ordering and completion.

Activity start/standby sequences run sequentially on each Amity. Follow the
original `ControllerImpl.set_activity()` in `~/projects-other/amity/hdmi.py`:
resolve the device, complete that step's original CEC work, then advance to the
next power/input step. Do not pre-resolve every activity device or let another
activity action overtake a sequence waiting for lookup or transmission
completion. Different Amities may execute their own sequences in parallel.

While an activity is being set, user key presses are discarded through the
existing `wait_for_release` gate before pressed-state registration. Keep that
gate effective throughout pending lookup and transmission steps; do not queue
presses, register ignored keys/tokens or create an ignored-press lifecycle.
A release with no registered pressed key is a no-op. Releases and exact cleanup
for keys registered before the transition retain their normal behavior. Keep
the adapters' existing physical edge recognition; unchanged held reports and
queued transition-period input must never replay a discarded press.
Once setup completes, retain the gate's original all-buttons-up clearing and
one-second fallback. Normal independent multi-key handling resumes after the
gate clears; do not serialize independent keys during normal operation.

The owner task must keep advancing incoming CEC, device resolution, completions
and release cleanup while the activity waits. Preserve the explicit per-key
amendments above. Store only transient operation progress
in the existing bounded typed communication mechanism; do not add an executor
queue of outgoing CEC frames, a hidden retry backlog or a persistent plan.
Test actual completion ordering across an Amity's front/back sides, a cold
lookup, start followed by standby, parallel owners and independent keys.

## Functional test framework

Harness code and bench wiring live in `cec-harness/`. The Python functional
framework MUST interact with DUT state through the same authenticated HTTP
endpoints used by the shipping Amity UI. Do not add firmware endpoints,
test-only commands, authentication bypasses or other DUT control surfaces for
functional tests. Extend the harness around the existing UI capabilities.

`cec-harness/TEST-HARNESS.md` is the source of truth for physical bench wiring.
Parse its HDMI-CEC mapping and setup/reset tables; do not duplicate GPIO maps or
persist virtual home-theater profiles. Each functional scenario owns its
components and releases their CEC addresses when it ends. The framework has
twelve functional scenarios: an HDMI scan on `amity-dut` with a TV, AVR and
playback device; `activity.py`, which reuses that scan to create, start and
repair the same activity before standby, checks explicit switch input, and tests
key routing and disconnected-client hold cleanup with equipment on the main
and on `amity-dut-2`; `source_theft.py`, which adds a second playback device
on AVR input 2 to check recovery from its Active Source announcement while the
input-1 player's activity remains selected; `add_amity.py`, which uses
`amity-dut` to add, verify, remove and reset the second board `amity-dut-2`;
and `consortium.py`, whose two-owner case adds that headless member and
exercises one TV/AVR/player triple on each owner in both activity/theft
directions, whose three-owner case adds `amity-dut-2` and `amity-dut-3`, and
whose four-owner case also adds `amity-dut-4`; plus `dual_display.py` for a
TV/projector activity and the isolated-source case in `source_theft.py` for
suppression before the announcement reaches the AVR.
`qualified_names.py` verifies exact routing of identical equipment names and
addresses on two owners, and `missing_equipment.py` verifies saved source
retention, numeric switch fallback, actual lookup cooldown and passive return.
`removal_cascade.py` removes an added member while four saved activities still
reference its equipment independently as source, display, audio and switch.
It rejects a stale confirmation after another local activity is created, then
requires a fresh review to delete exactly the affected activities and member
records while preserving unrelated configuration, ordering and slot metadata.
The retained local activity must still deliver fresh power/routing and standby
commands after removal. Never close the dependent activities before confirming
that removal; ordinary membership cleanup remains conservative.
`--test consortium` selects the two-owner scenario;
`--test consortium_three_duts` selects the three-owner scenario. `--test all`
runs all twelve scenarios, comprising seventeen test cases because `activity` has six.
Host-only framework unit tests are separate.
Do not retain the previous harness implementation in an archived directory.

The seventeen-case run on 2 November passed on unchanged firmware 0.1.129,
including stale removal rejection and an exact four-role cascade before any
dependent fixture cleanup. All 41 combined evidence/preservation checks and
529 framework unit tests pass. The final focused cascade rerun retains the
complete serialized review/deletion/settings/reset proof. Preserve the full
run under `test-results/removal-cascade-2026-11-02c/` and final focused evidence
under `test-results/removal-cascade-2026-11-02d/`, with their recorded source
provenance and scope. No firmware update was required for this test-only change.

The stricter dual-display handoff observes original physical frames before the
action, rejects transient power commands to shared AVR/player equipment, and
requires completed old-display Standby, selected-display power-on and AVR
routing in that order. Drain each involved receive stream through the action
assertion boundary, and account for per-wire SSH clock uncertainty. Complete
Linux RX timestamps establish frame-completion order; do not claim proof of
cross-wire first-edge timing or non-overlap.

The missing-equipment scenario must establish a genuinely absent source cache
entry through explicit scan after withdrawing its responder. Repeated actual
starts retain the selector and use switch fallback without cooldown scans;
wait the real 120 seconds, prove expiry did not launch deferred work, then
require a new miss to scan only the back side and preserve the front cache.
Passive return requires physical announcement/targeted identity evidence and
the correct UI observation without another full scan; unchanged activity power,
routing and keys must work afterward. These are test evidence requirements,
not new firmware availability gates.

The expanded sixteen-case physical run on 1 November passed fifteen cases;
missing-equipment recovery failed its final Select assertion with a one-attempt
CEC LineError. An unchanged diagnostic retry also had a local-key LineError and
was interrupted after cleanup; its USB capture unexpectedly reset the main.
Do not claim a complete-suite pass from these runs. Preserve their evidence in
`test-results/functional-expansion-2026-11-01/` and earlier results with their
original scope. Final saved configuration and pairing preservation checks pass.

On 2 November, all sixteen cases passed on exact firmware 0.1.129 on all four
bench boards. The SDK's 256-byte C2M cache-writeback chunks address the measured
CEC timer delay without changing CEC attempts or activity algorithms. Lookup
evidence distinguishes announcement-correlated identity queries from scans;
boot/generation and unannounced-query checks remain mandatory. All 35 final
verification checks pass, including twelve member removal/reset receipts,
twenty-four test-owned activity cleanups, retained main boot/configuration/
pairings and all twenty-four theft responses within 66.899–69.600 ms. Evidence
is `test-results/functional-verification-2026-11-02b/all.json` and
`all-verification.json`. The native host suite and all 486 framework unit tests
also pass. Preserve the earlier failures and the separately interrupted
974-pair stress run with their original scope.

Keep enrollment/scan/activity operation deadlines separate from CEC assertions.
`RunConfig.timeout` / `--timeout` defaults to 180 seconds, based on the measured
95.494-second fully verified Add;
`RunConfig.cec_timeout` / `--cec-timeout` independently defaults to five seconds
for ordinary fresh power/routing waits. Raising the operation budget must not
raise the CEC wait budget. Source-theft remediation has a hard 250 ms
physical wire-completion limit, not the operation or ordinary CEC timeout.
The directed Standby and resulting state must be proven within 250 ms of
the same component's successful physical Active Source transmission. The
shared-wire scenario also requires route restoration within that same original
window; isolated consortium wires do not add a route-restoration requirement.
Use complete Linux CEC TX/RX timestamps from the same wire and correlate the
trigger to its exact component/session. Record actual response latency. A late
response already in history, virtual traffic or a local state change cannot
pass. Slow authenticated owner proof may validate timely evidence later, but
must not extend or restart the physical response window. 250 ms is an
absolute maximum; expected normal remediation is substantially faster.

Keep functional test bodies limited to equipment, topology and behavior:
construct named DUTs/components, enter `TestBed`, then call operations such as
`dut.scan_hdmi().assert_contains(...)`. Named DUT construction parses wiring
implicitly. Shared `config.py`, `testbed.py`, `runner.py` and `discovery.py`
handle runtime options, per-DUT credentials/URLs/TLS, authentication, identity
checks, cleanup, UI operations, fresh-wire evidence and reports. Do not put
wiring parsers, credentials, CLI/report helpers or protocol handlers into a
functional scenario. `TestBed` supports multiple DUTs through the same resource
interface and always attempts cleanup; component `handle()` implementations
remain in the common framework.

`TestBed(..., members=(secondary,))` permits components on an explicitly named
headless member without authenticating or contacting that board's HTTP UI.
`control_only` remains physical-button control without CEC components.
The main and member must belong to the same active bed and shared CEC session.
Before including remote components in the main's scan, use the existing Add
flow and `member.assert_connected()` to prove the member's exact identity,
boot, assigned name, ready membership, configuration and settings. Capture
`ScanResult.owner_boots` for the main and member; recheck current main/group and
member readiness/boot against that proof for distributed scan/activity evidence.
Require each component's fresh discovery exchanges on its actual owner's GPIO
and correlate the existing UI's typed owner/tentacle/side observations. Another
owner's matching label, address or cached observation cannot prove that wire.
Activity boundaries and remote power/routing assertions retain the same owner
boots and initiators. These are test-evidence checks, not additional runtime
availability gates or firmware APIs.

Model actual HDMI attachments explicitly: the TV connects to a tentacle's
front endpoint, the AVR to its back endpoint, and playback to an AVR input.
Children inherit the attachment's DUT, tentacle, CEC side and GPIO. The DUT is
inline for HDMI physical addressing and consumes no extra address level;
front/back CEC isolation remains intact. Do not substitute a direct TV/AVR
component-parent edge for the intervening DUT connection.

The initial scan must explicitly assert both presence and exact membership of
TV, AVR and playback, with no additional or duplicate scan entries. Assert the
TV on tentacle 0/front and AVR/playback on tentacle 0/back using actual typed
owner/adapter/side observations from existing UI APIs. Fixture attachments,
matching names, counts or human-readable location strings alone do not prove
that the DUT reported the correct connections.

The activity test must first assert all three components are in Standby.
Starting the saved activity must deliver a fresh physical DUT-origin power-on
command to every component and leave all three On. Returning to standby must
deliver a fresh CEC Standby command to every component and leave all three in
Standby. Local state setters, stale traffic and virtual peer messages cannot
satisfy those command assertions. `Off` means electrically unpowered with no
CEC traffic; ordinary CEC Standby retains the powered CEC responder. Reuse the
existing authenticated UI activity operations and clean up test-owned activity
configuration before closing component responders and logging out.

The activity scenario also has a separate explicit switch-input sub-test,
run together with the power-cycle case by `--test activity`. Use the AVR as
the switch and no CEC source, preserving the original fallback behavior;
a discovered CEC source uses Set Stream Path instead. Assert the activity is
active, TV/AVR are On, the unselected player remains in Standby, and a fresh
directed DUT Select AV Input Function with the configured numeric input was
actually applied by the AVR. Local state, virtual traffic and a physical-address
routing command cannot satisfy that input assertion. Keep numeric input state
separate from the component's CEC physical-address routing state.

The source-theft test uses two playback devices attached to AVR inputs 1 and 2,
with a saved activity for each using the shared TV and AVR. The second
`PlaybackComponent` explicitly uses `address_type="tuner"` for logical-address
allocation while retaining playback behavior; the three-component back bus
uses the explicit CEC 1.4 profile. Its extended scan must require exactly all
four components and assert the TV on tentacle 0/front and the AVR and both
players on tentacle 0/back. Start and verify the first
player's activity, then use the second player's `activate()` to send a physical
Active Source broadcast. `Component.activate()` returns the transmission
result. The DUT must issue a fresh directed Standby to the second player and a
fresh Set Stream Path to the first player's physical address;
`avr.wait_for_input(player, since=boundary, after=announcement, within=0.250)` must verify actual DUT wire
evidence. The first player must be active again, and `activity.assert_active()`
verifies its selected UI activity and DUT boot did not change. Clean up both
test-owned activities before component responders close. Keep raw CEC and
protocol handling in shared component helpers, outside the functional scenario.

The Add Amity test uses `TestBed(main, candidate, control_only=(candidate,))`
with `DUT("amity-dut")` and `DUT("amity-dut-2")`. Only the original main needs
HTTP credentials. The candidate uses its documented button wiring without a
Wi-Fi/UI prerequisite. `main.add_amity(candidate)` owns a 3.5-second candidate
FW RESET hold and the existing UI scan/add/operation flow. Resolve FW RESET
GPIO assignments only by parsing the setup/reset table in `TEST-HARNESS.md`;
never hardcode bench pin assignments in framework or test code.
`--harness-host` selects Pi-owned bounded physical button control as well as
CEC transport; SSH button requests carry the already resolved GPIO.
The normal admission profile uses no setup code. Keep HTTP and protocol details
in shared helpers. `member.assert_connected()` must verify full inventory ID,
assigned name, ready membership, configuration and device settings while
preserving the original main, its authority and existing activities.

A single normal-profile Add-only physical measurement on 29 October 2026 with
firmware 0.1.99 took 95.494 seconds from helper entry to fully verified ready,
59.272 seconds from Add POST to membership-operation completion and 67.940
seconds from Add POST to verified settings/ready. It used the original main and
`amity-dut-2`, without CEC component traffic, included host HTTP time, and ended
with reset `scheduled` cleanup. Evidence is
`test-results/device-removal-2026-10-29/enrollment-measurement.json` and
`enrollment-timing.json`. This enrollment timing does not relax CEC deadlines
or qualify distributed power/source-theft behavior.

The user explicitly authorized reset on removal as this test's cleanup. Remove
only the known newly added member through the existing UI Remove API, with the
reviewed configuration/presentation and membership revisions, target boot and
affected activity list, and require acknowledgment that
the candidate reset was scheduled. Never remove the main or a preexisting
member, and do not add a ten-second factory reset before the test. Uncertain
ownership or a missing reset acknowledgment fails cleanup and must be reported.
Reject a fixture candidate with preexisting referenced user activities before
opening its Add window. Cleanup must delete its saved owner, settings, equipment
and name without deleting unrelated data or accepting another testbed's receipt.
An Add-only result with mixed firmware versions does not qualify distributed
power behavior; do not invent a global firmware-version equality gate.

The consortium scenario uses `amity-dut` and `amity-dut-2`, each with a TV on
tentacle 0/front and an AVR plus input-1 player on tentacle 0/back. Scan exactly
all six identities over the four isolated wires, then save one activity per
triple. Exercise main-active/member-inactive and member-active/main-inactive.
For each cycle assert all six begin in Standby, the active three receive fresh
physical power-on commands, and the active AVR selects its player. After the
other owner's player sends a physical Active Source announcement, require a
fresh physical directed CEC Standby to that thief and assert all three of that
owner's components are in Standby. The only source of truth is the current
activity, with the special standby activity when the whole system is in standby.
Its assigned qualified source determines the owner-local thief mismatch;
global standby permits no active source. Do not add per-owner inactive state
or flags. The active owner must retain its power, route, active player and
selected UI activity/boot; isolated inactive
wires do not require a new Set Stream Path on the unaffected active owner.
Returning the selected activity to standby requires fresh CEC Standby on its
active three and leaves all six in Standby. Keep the existing shared-wire
source-theft test's directed Standby and route-restoration requirements.
Clean up both activities and unreferenced fixture equipment before removing
the known added member, with responders still running. Preserve normal reserved
activity-slot tombstones; accept only new default-metadata tombstones for known
successfully created and removed fixture activity IDs, without altering prior
slots or user metadata. On 29 October 2026 all six functional cases passed on
`test2` against exact firmware 0.1.98 on all four bench DUTs, including both
directions of this two-owner scenario, six typed scan locations and fourteen
fresh physical power-command assertions. All testbeds cleaned up without
errors, including acknowledged member removal/reset. Evidence is
`test-results/consortium-live-config-2026-10-29/functional.json`; its deployment
and final main preservation reports are alongside it. This qualifies only the
exercised normal-profile two-board, one-adapter-per-board simulated CEC setup;
do not extend historical Add-only results or this result to other hardware.

The separate `test_consortium_three_duts` case adds `amity-dut-2` and
`amity-dut-3` to `amity-dut` in the same bed. It uses one TV/AVR/player triple
per owner, requires exactly nine identities and typed locations on six isolated
wires, and exercises each owner as active with a physical theft attempt from
each of the other two owners. Preserve the same fresh power/standby evidence,
active route/activity/boot checks and all-components-in-Standby boundaries as
the two-owner case. Clean up all three activities and fixture equipment before
removing/resetting member 3 and then member 2, with responders still running.
Verify member 2 before adding member 3 and again after removing member 3;
the shared discovery and activity evidence rechecks both proven owner boots
while both are enrolled. Host scenario checks do not establish physical
three-owner qualification or extend the historical two-owner results.

The initial physical qualification on 29 October 2026 failed when the
candidate's 0.1.84 `fw_reset` task overflowed during the 3.5-second gesture;
the candidate was not discovered, Add/Remove were not submitted, and scan
cancellation completed. Preserve that and the later failed debug reports as
historical failures. After the documented fixes, two consecutive unchanged
original tests pass on main 0.1.94/candidate 0.1.93: Add, exact connected/settings
checks and scheduled-reset removal, with no retained member or cleanup errors.
The candidate returns to unclaimed Ready/automatic Matter setup without panic,
and final HTTP checks retain the main boot/authority, exact configuration and
two Matter fabrics. The full host runner passes all 240 framework unit tests.
Evidence is `test-results/add-amity-debug-2026-10-29/add-amity-main94-instrumented.json`
and its two functional reports and final main snapshot. This qualifies that
pair's normal-profile Add/removal path, not CEC/AV or the strict setup-code
profile. At that functional-run checkpoint boards 3/4 were on 0.1.88.
A subsequent application-only update puts all four DUTs on the exact 0.1.94
image, with main skipped and verified unchanged; it does not rerun or extend
the earlier main-0.1.94/candidate-0.1.93 functional qualification. Deployment
evidence is `test-results/all-dut-update-2026-10-29/deployment.json`. No API
or assertion criterion was weakened.

Components sharing a physical harness GPIO also share one virtual CEC bus.
Route directed virtual messages and replies within that bus, with powered ACK,
Off NACK and no Off RX/hooks; polls report presence without RX handling. Keep
separate GPIOs isolated even when components share topology parent links.
Broadcast to virtual peers and the physical DUT wire independently, once each;
report the actual wire result once. Mark TX/RX evidence as virtual or wire;
virtual records have no wire timestamp and cannot prove DUT behavior. Hooks
and nested replies remain synchronous, bounded at 32 nested deliveries to reject
echo loops, with no additional queue or delay. Physical Linux backend limits
must not disable virtual component-to-component communication.
