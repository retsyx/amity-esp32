# Preserve original Amity behavior

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
four functional scenarios: an HDMI scan on `amity-dut` with a TV, AVR and
playback device; `activity.py`, which reuses that scan to create, start and
standby an activity; `source_theft.py`, which adds a second playback device
on AVR input 2 to check recovery from its Active Source announcement while the
input-1 player's activity remains selected; and `add_amity.py`, which uses
`amity-dut` to add, verify, remove and reset the second board `amity-dut-2`.
`--test add_amity` selects that scenario; `--test all` runs all four.
Host-only framework unit tests are separate.
Do not retain the previous harness implementation in an archived directory.

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
`avr.wait_for_input(player, since=boundary)` must verify actual DUT wire
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

The user explicitly authorized reset on removal as this test's cleanup. Remove
only the known newly added member through the existing UI Remove API, with the
reviewed membership revision and target boot, and require acknowledgment that
the candidate reset was scheduled. Never remove the main or a preexisting
member, and do not add a ten-second factory reset before the test. Uncertain
ownership or a missing reset acknowledgment fails cleanup and must be reported.
An Add-only result with mixed firmware versions does not qualify distributed
power behavior; do not invent a global firmware-version equality gate.

Physical qualification on 29 October 2026 failed when the candidate's
0.1.84 `fw_reset` task overflowed during the 3.5-second gesture. The candidate
was not discovered; Add and Remove were not submitted, and scan cancellation
completed. All 238 framework unit tests passed. Connected membership and the
removal/reset acknowledgment remain physically unqualified; no firmware change
or weaker existing-API assertion was introduced for this result.

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
