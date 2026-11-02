# Original activity handling with owner delivery

**27 October 2026 user amendment.** The local Amity algorithms remain the activity model. Distribution adds a stable equipment owner and transports original intents; it does not introduce persisted executable plans or a replacement activity planner. [AGENTS.md](../AGENTS.md), [addressing](03-domain-model-and-addressing.md), [CEC behavior](06-cec-roles-and-message-policy.md) and [transport](08-consortium-transport-and-routing.md) are normative.

## 1. Saved choices and live execution

An activity retains name, display, source, audio, optional switch device and switch input. Equipment choices resolve through `Amity device ID / HDMI adapter / CEC side / OSD name`. Stable IDs preserve labels and external integration identity; activity order drives the original remote shortcuts. These records are not route-approval tokens.

There is no persistent `enabled` flag, approved route, physical cable graph, capability gate, resource reservation or saved physical-address evidence needed to execute an activity. The chosen owner, adapter and name either resolve under the original local rules or they do not. Missing equipment stays saved. Its return does not require a user repair/approval operation.

## 2. Reuse the original decisions

The original controller sequences the existing activity decisions relative to the current activity: selective power-off/on, deduplication of equipment used in several roles, original source routing or explicit switch-input fallback, audio status handling, standby and same-target repair. Preserve the same original decisions for local and remotely owned recipients.

Distribution delivers the shared activity selection and saved choices to each Amity. Each Amity runs the original local activity algorithm for its own equipment, independently of the other owners. Its next local action waits for its own lookup and CEC transmission completion. Do not orchestrate remotely prescribed CEC steps, wait for another owner's device completion, or add a global phase barrier. Owner-local source lookup determines its original routing/fallback behavior.

**29 October 2026 activity-sequencing amendment.** Start and Standby follow the original Python `ControllerImpl.set_activity` sequence at each owning Amity: look up the next recipient through `get_device`, wait for its original command(s) to complete, then advance to the next recipient. Resolve devices just in time; do not pre-resolve all activity roles. Preserve old-only power-off, new-only power-on, source input or explicit switch fallback, and the final current-activity update. Even an already-standby force sequence completes the front broadcast before sending the back broadcast. A later local Start, Standby or same-target repair cannot overtake an earlier activity step paused for lookup or transmission completion on that owner. Amities synchronize the activity start/stop intent, then run independent owner-local sequences. Devices on one Amity must not wait for lookup, transmission or completion on another. Old-only Standby precedes new-only power-on within each owner; shared devices retain baseline selective-power behavior.

**1 November 2026 clarification.** Starting an activity blocks its sequence at the current lookup or CEC transmission. It does not commit early and keep an independent lookup-result ledger. A later Start, repair or Standby stays behind it. After transmitting an activity command, never wait for Report Power Status, Active Source, audio status, another application reply, or a quiet-response interval to advance. Those observations may still update ordinary runtime state. The shared resolver retains its original discovery behavior on a cache miss.

The owner keeps advancing RX, completions and the original input gate while an activity waits. Key presses during activity setting are ignored before pressed-state registration; a later release for an unregistered press is a no-op. Do not add ignored-press bookkeeping or replay swallowed input. Preserve the existing physical-input gate's all-buttons-up clearing and one-second fallback. Once the gate clears, normal independent multikey/completion behavior resumes; keys never queue behind activity work. Accepting a transition ends previously registered holds and fences their pending lookup work and queued user presses before a first wire edge. A frame already transmitting can finish. Resolve release cleanup against the old current activity before this owner advances initialization; another owner does not wait for it. A transient program counter retains only the original function's choices, loop cursor and next step in existing bounded semantic IPC. It never stores outgoing CEC frames, a compiled plan or persistent resume state, and it never repeats already attempted side effects. Reactive received-frame continuations retain their separate guarded step; they cannot overwrite a paused user activity or replay an old implicit Standby after a new activity context.

Preserve the original treatment of absent source/display/audio/switch equipment and partial progress. A failed lookup must not become a persistent activity veto or a new all-or-nothing transaction. Preserve original configure/received-frame effects, transmission-paced input readiness, same-target selection and long-Power repair, with the explicit [29 October scan/cache amendment](05-discovery-and-topology.md#2-scan-triggers-and-retained-discovery). The distributed layer adds no normal-operation delay, retry cadence, compensation sequence, automatic failover or automatic activity resumption.

## 3. Put work at the owner

Each adapter's owner maintains its original discovery cache, allocated identities, physical-address selection and local CEC execution. A controller-side lookup selects the authenticated owning Amity; adapter/side/OSD resolution belongs to that owner. Static user choices are not replaced by copied logical addresses.

The 1 November isolated-source topology adds an owner-local input step. Python
`ControllerImpl.set_activity_input()` and `cec.DeviceImpl.set_stream_path()`
send only Set Stream Path on a resolved source's bus; they never announce on
the isolated paired front after that successful lookup. That leaves an AVR
input unselected when an additional inline adapter isolates the source from
the AVR. The activity retains its original qualified
display selector at the source owner. When that display is on another owner/adapter, the source is
on Back, and this adapter's own Front PA is nested below a switch (for example,
1.2.0.0), its owner first resolves the source, announces Active Source with its
own PA on its paired Front, then completes the ordinary Back Set Stream Path.
Keeping Set Stream Path last lets the owner apply the actual completed activity
before processing a causal source announcement. This is completion ordering,
not a wait for the source to answer and not a pending-source exception.
Both transmissions complete before
the next activity operation. Neither waits for an application reply from the source. The existing bounded request retains
only the next semantic step, rechecks authority, and never retries a rejected
CEC submission. Rejected Front admission does not prevent the original Back
operation, and an unresolved source never sends this Front announcement.
No received source announcement is forwarded, and source-theft
decisions continue to use only the current activity.

Same-adapter source/display handling retains the original transcript. A direct
display connection such as 1.0.0.0 also emits no extra announcement when another
display is selected, so a dual-display activity leaves its unselected TV alone.
The runtime PA selects the upstream routing command; it is not saved topology
approval or an availability gate, and it never blocks the original lookup,
power or Back Set Stream Path. `tests/activity_sequence_test.cpp` checks the
actual owner worker's nested/direct/same-adapter/front-source cases, exact wire
and PA, completion order, once-only submission, rejected Front admission,
unresolved source handling and authority loss between steps.

The 1 November 2026 physical 0.1.117 run verifies this nested-input adaptation:
three isolated theft attempts are suppressed on both AVR wires, while selecting
that source produces a fresh Front Active Source and powers its required
devices. The unchanged dual-display case also passes with the unused display
in Standby. All ten functional cases pass with clean teardown in
`test-results/source-theft-isolated-2026-11-01/full117-final.json`;
preservation and exact wire timing are summarized in
`full117-final-verification.json` alongside it. This is simulated CEC evidence,
not HDMI video qualification.

Every device access calls the same owner-local resolver described in [discovery](05-discovery-and-topology.md#one-device-access-path): cache lookup, throttled side scan on a miss, then the same cache again. Activity, key and received-frame callers do not inspect cache availability or choose a passive/cache-only variant. They may wait on an opaque lookup continuation while the owner keeps advancing input, RX and CEC completions. The blocked local Source step uses its actual local lookup result for the original routing/fallback decision; it does not ask another owner for permission or a CEC-step result.

A one-shot operation is not retained for later replay when an owner reconnects. An uncertain relative action is not retried under a new identity. Unavailable equipment receives the original failure/discovery handling. Network loss does not cause a headless member to choose an activity independently.

## 4. Remote input and held keys

Keep the original controller's input and gesture algorithms, startup/selection wait-for-release gate, one-second fallback, Siri-only Power/selector macros and exact swipe sequences. Transport button state changes. Keep per-key completion-paced repetition at the emitting owner with one attempt per key press. A failed key ends its state; repeated unchanged down reports do not revive it.

**1 November 2026 key-routing amendment.** The main retains only the receiving Amity index (0–3) for each pressed raw key. It transports key-down/key-up edges and the activity lifetime to that owner. Each owner already has the full activity description and chooses its local audio or source role. It resolves through the shared resolver for each key action, including release. Do not retain a per-key resolved adapter/side/CEC recipient or substitute an old address when the current lookup differs. The resolver keeps its cache and original scan/cooldown behavior.

One owner-level table tracks key lifecycle, a held key's translated Play/Pause opcode, failure and local completion progress. An admitted key whose CEC attempt fails stays ended until its matching up; unchanged down reports cannot revive it. An up with no registered key is a no-op. Final release is operand-free and aggregates the remaining keys by their currently resolved physical wire, initiator and destination. Ending one key must not release another active key on that recipient. A finite tap transfers a bounded atomic down/up pair, preserving one press even if transport delivery is delayed. No periodic press or permission-renewal messages originate from the main.

When a transition is accepted on an Amity, stop old holds and invalidate old pending user-key lookups and queued user presses immediately. Already-on-wire frames finish; no further old user press may start. Old up events, lookup callbacks and transmission completions cannot affect a new hold. This cutoff does not reorder earlier local activity device steps or create a cross-Amity CEC barrier. Retain the setup input gate, all-up clearing and one-second fallback; swallowed holds never replay after setup. Local transmission instances are completion bookkeeping, not transported key tokens or replay identifiers.

The owner continues input, RX, resolver work and completions while a key action waits for lookup. Necessary bounded typed IPC retains semantic progress only. `tests/owned_hub_test.sh` covers original key behavior plus these explicit amendments, including independent keys, failure suppression, changed lookup results and activity transitions.

## 5. Queues and completion

Use bounded IPC where thread/task communication requires it. Preserve causal down/up ordering and failed-key no-op behavior under pressure. The executor submits directly to CEC and does not retain ordinary work for capacity. Only CEC has a transmit FIFO: eight total active/queued frames per wire, including releases. Every full-FIFO TX drop is logged; a dropped release remains dropped.

Each owner tracks only its own original activity progress and local transmission completions. The group transport synchronizes activity intent, not CEC-step completion. A blocked lookup or transmission on one owner must not stall another owner. This is transient local bookkeeping, not a distributed CEC planner, persistent state or an outgoing queue. Key presses accepted by the original input gate and ordinary commands submit immediately without waiting for another key's completion or CEC capacity. During activity setting the gate discards user presses as specified above; it does not queue them behind setup work.

Submission, CEC admission, transmission completion and equipment state remain different outcomes. A successful transport receipt is not proof of a CEC ACK or picture/audio. These distinctions do not justify a persistent approval barrier.

## 6. Restart and configuration

Persist activity configuration, identity/trust and necessary crash-consistent configuration/reset records. Do not persist an executable queue, current-activity resume journal, active holds, discovered addresses or execution grants. Reboot does not replay missed input. Keep the baseline's normal startup behavior and explicit user activity controls.

An edit validates the user's fields and selectors without requiring current equipment presence. Preserve the original configure behavior: every accepted configuration application performs the original standby cleanup, clears the selected activity and advances its generation, including label-only edits. The sequential cleanup completes before the replacement activity records take effect. Do not add an in-place presentation optimization that retains running activity/held state. Discovery refreshes runtime lookup; it does not rewrite activity availability. Configuration CAS and authenticated delivery prevent stale edits without compiling an owner-specific topology approval.

**29 October 2026 scan/cache amendment.** Applying an activity configuration does not disconnect members, discard native HDMI device records or remote observations, prune unreferenced adapters, or request enumeration. It preserves an independently running scan. Boot and explicit UI requests may scan; actual communication to an uncached recipient may scan only its side under that side's 120-second cooldown. Boot/UI scans bypass eligibility but refresh their side timestamps. Reconnects, periodic timers and cooldown expiry alone do not scan or replay failed work. An original old-recipient cleanup command can still encounter a genuine cache miss and use that same communication lookup rule.

## 7. Acceptance evidence

The decisive test is equivalent original behavior. For the same saved activity/input/observations, compare baseline local execution, qualified all-local execution and execution with each role moved to another owner. Assert frames, ordering, attempts, admission and key lifecycle. Include AVR audio, explicit receiver inputs, missing OSD names and return, duplicate scoped names, LA change while held, failed TX, full FIFO and scan progress while transmissions remain outstanding. See [validation](15-validation-and-release-gates.md).

`tests/cec_activity_sequence_test.cpp` checks the native program counter's just-in-time lookup/TX order, switch fallback, once-only effects, configuration cleanup, idle Standby and independent keys after completion. `tests/activity_sequence_test.cpp` exercises the real owner worker without response-based settling, including queued activity selections, side/adapter completion order, parallel owners, the input gate, normal independent key streams, old-revision cleanup, stale reactive Standby and an immediate Active Source reply following Set Stream Path completion.
