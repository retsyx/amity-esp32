# Original activity handling with owner delivery

**27 October 2026 user amendment.** The local Amity algorithms remain the activity model. Distribution adds a stable equipment owner and transports original intents; it does not introduce persisted executable plans or a replacement activity planner. [AGENTS.md](../AGENTS.md), [addressing](03-domain-model-and-addressing.md), [CEC behavior](06-cec-roles-and-message-policy.md) and [transport](08-consortium-transport-and-routing.md) are normative.

## 1. Saved choices and live execution

An activity retains name, display, source, audio, optional switch device and switch input. Equipment choices resolve through `Amity device ID / HDMI adapter / CEC side / OSD name`. Stable IDs preserve labels and external integration identity; activity order drives the original remote shortcuts. These records are not route-approval tokens.

There is no persistent `enabled` flag, approved route, physical cable graph, capability gate, resource reservation or saved physical-address evidence needed to execute an activity. The chosen owner, adapter and name either resolve under the original local rules or they do not. Missing equipment stays saved. Its return does not require a user repair/approval operation.

## 2. Reuse the original decisions

The original controller sequences the existing activity decisions relative to the current activity: selective power-off/on, deduplication of equipment used in several roles, original source routing or explicit switch-input fallback, audio status handling, standby and same-target repair. Preserve the same original decisions for local and remotely owned recipients.

Deliver each typed intent in the original order to the selected owner. The owner performs the actual local lookup and local CEC handling. Do not centralize discovered CEC addresses or translate a semantic operation into a new remotely prescribed CEC policy. Do not wait for all owners to acknowledge or reserve a global plan before an independent owner can proceed. A source-input result returns its original generation/sequence and actual presence so the controller continues the original switch fallback/audio query; each owner's actual activity steps obey the sequential completion rule below.

**29 October 2026 activity-sequencing amendment.** Start and Standby follow the original Python `ControllerImpl.set_activity` sequence at each owning Amity: look up the next recipient through `get_device`, wait for its original command(s) to complete, then advance to the next recipient. Resolve devices just in time; do not pre-resolve all activity roles. Preserve old-only power-off, new-only power-on, source input or explicit switch fallback, and the final current-activity update. Even an already-standby force sequence completes the front broadcast before sending the back broadcast. A later Start, Standby or same-target repair cannot overtake an earlier activity step paused for lookup or transmission completion on that owner. Different Amities may advance independently; no cross-owner completion barrier is added.

The owner keeps advancing RX, completions and the original input gate while an activity waits. Key presses during activity setting are ignored before pressed-state registration; a later release for an unregistered press is a no-op. Do not add ignored-press bookkeeping or replay swallowed input. Preserve the existing physical-input gate's all-buttons-up clearing and one-second fallback. Once the gate clears, normal independent multikey/completion behavior resumes; keys never queue behind activity work. Already registered key streams retain their original exact-recipient cleanup and failed-TX state. A transient program counter retains only the original function's choices, loop cursor and next step in existing bounded semantic IPC. It never stores outgoing CEC frames, a compiled plan or persistent resume state, and it never repeats already attempted side effects. Reactive received-frame continuations retain their separate guarded step; they cannot overwrite a paused user activity or replay an old implicit Standby after a new activity context.

Preserve the original treatment of absent source/display/audio/switch equipment and partial progress. A failed lookup must not become a persistent activity veto or a new all-or-nothing transaction. Preserve original configure/received-frame effects, activity settling, same-target selection and long-Power repair, with the explicit [29 October scan/cache amendment](05-discovery-and-topology.md#2-scan-triggers-and-retained-discovery). The distributed layer adds no normal-operation delay, retry cadence, compensation sequence, automatic failover or automatic activity resumption.

## 3. Put work at the owner

Each adapter's owner maintains its original discovery cache, allocated identities, physical-address selection and local CEC execution. A controller-side lookup selects the authenticated owning Amity; adapter/side/OSD resolution belongs to that owner. Static user choices are not replaced by copied logical addresses.

Every device access calls the same owner-local resolver described in [discovery](05-discovery-and-topology.md#one-device-access-path): cache lookup, throttled side scan on a miss, then the same cache again. Activity, key and received-frame callers do not inspect cache availability or choose a passive/cache-only variant. They may wait on an opaque lookup continuation while the owner keeps advancing input, RX and CEC completions. A source-input result still reflects actual owner-local lookup, preserving the original explicit-switch fallback.

A one-shot operation is not retained for later replay when an owner reconnects. An uncertain relative action is not retried under a new identity. Unavailable equipment receives the original failure/discovery handling. Network loss does not cause a headless member to choose an activity independently.

## 4. Remote input and held keys

Keep the original controller's input and gesture algorithms, startup/selection wait-for-release gate, one-second fallback, Siri-only Power/selector macros and exact swipe sequences. Transport button state changes. Keep per-key completion-paced repetition at the emitting owner with one attempt per key press. A failed key ends its state; repeated unchanged down reports and authority refresh do not revive it.

An admitted hold retains its exact recipient even when later name lookup changes. Final release is a recipient-stream decision over all keys on the same physical wire, initiator and destination. Exact cleanup, stale-input generation checks and bounded causal IPC remain necessary. They are transient lifecycle state, not a durable activity graph.

The controller retains a hold token only after owner delivery is consumed. Local adapter/authority rejection before that admission leaves no token; its button-up is a no-op and the first fresh press after the adapter returns reaches the owner. A consumed down whose CEC attempt fails still retains its lifecycle token until physical up so unchanged down reports cannot restart it. `tests/owned_hub_test.sh` exercises the actual qualified local path through detach, rejected down/up, reattach and the first fresh down, followed by failed-TX suppression and a later new press. This closes the rejected-local-token gap without changing the baseline's per-key completion or release behavior; the existing local/remote transcript and settling comparisons remain required.

## 5. Queues and completion

Use bounded IPC where thread/task communication requires it. Preserve causal down/up ordering and failed-key no-op behavior under pressure. The executor submits directly to CEC and does not retain ordinary work for capacity. Only CEC has a transmit FIFO: eight total active/queued frames per wire, including releases. Every full-FIFO TX drop is logged; a dropped release remains dropped.

Owners report volatile setup generation, last submitted sequence and pending admitted-frame completions so the original controller preserves the baseline activity-settling latch. This is necessary completion bookkeeping for already submitted work, not a prepared plan, persistent state or an outgoing queue. Key presses accepted by the original input gate and ordinary commands submit immediately without waiting for another key's completion or CEC capacity. During activity setting the gate discards user presses as specified above; it does not queue them behind setup work.

Submission, CEC admission, transmission completion and equipment state remain different outcomes. A successful transport receipt is not proof of a CEC ACK or picture/audio. These distinctions do not justify a persistent approval barrier.

## 6. Restart and configuration

Persist activity configuration, identity/trust and necessary crash-consistent configuration/reset records. Do not persist an executable queue, current-activity resume journal, active holds, discovered addresses or valid execution grants. Reboot does not replay missed input. Keep the baseline's normal startup behavior and explicit user activity controls.

An edit validates the user's fields and selectors without requiring current equipment presence. Preserve the original configure behavior: every accepted configuration application performs the original standby cleanup, clears the selected activity and advances its generation, including label-only edits. The sequential cleanup completes before the replacement activity records take effect. Do not add an in-place presentation optimization that retains running activity/held state. Discovery refreshes runtime lookup; it does not rewrite activity availability. Configuration CAS and authenticated delivery prevent stale edits without compiling an owner-specific topology approval.

**29 October 2026 scan/cache amendment.** Applying an activity configuration does not disconnect members, discard native HDMI device records or remote observations, prune unreferenced adapters, or request enumeration. It preserves an independently running scan. Boot and explicit UI requests may scan; actual communication to an uncached recipient may scan only its side under that side's 120-second cooldown. Boot/UI scans bypass eligibility but refresh their side timestamps. Reconnects, periodic timers and cooldown expiry alone do not scan or replay failed work. An original old-recipient cleanup command can still encounter a genuine cache miss and use that same communication lookup rule.

## 7. Acceptance evidence

The decisive test is equivalent original behavior. For the same saved activity/input/observations, compare baseline local execution, qualified all-local execution and execution with each role moved to another owner. Assert frames, ordering, attempts, admission and key lifecycle. Include AVR audio, explicit receiver inputs, missing OSD names and return, duplicate scoped names, LA change while held, failed TX, full FIFO and scan progress while transmissions remain outstanding. See [validation](15-validation-and-release-gates.md).

`tests/cec_activity_sequence_test.cpp` checks the native program counter's just-in-time lookup/TX order, switch fallback, once-only effects, configuration cleanup, idle Standby and independent keys after completion. `tests/activity_sequence_test.cpp` exercises the real owner worker at zero settling delay, including queued activity selections, side/adapter completion order, parallel owners, the input gate, normal independent key streams, old-revision cleanup, stale reactive Standby and an immediate Active Source reply following Set Stream Path completion.
