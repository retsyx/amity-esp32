# Preserve original Amity behavior

The original Amity algorithms and functionality at commit
`d50af8026938797fc0820d0107f4c4e064c2d2bc` are the authoritative baseline.
Preserve them unless the user explicitly requests a change. This rule applies
to implementation, design documents, tests, and future maintenance.

The consortium is an additional ownership and transport layer around the
original algorithms. It is not permission to redesign activity planning,
AVR/audio/switch handling, discovery, physical-address selection, remote input,
CEC transmission, retries, held-button repetition, or existing UI workflows.
Keep existing behavior for both local and remotely owned equipment. Make only
the adaptations required to identify the owning device, deliver the original
intent, and fence stale or unauthorized distributed work. Do not introduce new
normal-operation timing, timeouts, cadence, or recovery behavior without an
explicit user request.

For held buttons, preserve the original completion-paced loop at the emitting
executor: send the next press after the preceding attempt completes and only
when no command is waiting. Communicate button state changes between Amity
devices, not periodic presses or per-button renewals.

The following are explicit user changes to the baseline, dated 21 October 2026:

- Every CEC key-press frame uses one wire attempt; remove key-press retries.
- Any failed key-press transmission ends that key's pressed state and stops its
  repetition. A later matching button-up is a no-op. Unchanged down reports and
  authority renewal must not restart it.
- CEC release is implicit at the end of the executor's press stream, after one
  or more presses, when no keys remain pressed for that receiving CEC device.
  Remote button-up updates state; it is not a direct key-specific release TX.
  Release has no key operand. Aggregate keys per physical wire, initiator and
  destination; ending one key must not release another active key there.
  A last-key TX failure may therefore cause a final release attempt. Retain
  exact safe stream cleanup state separately from the ended key's token.
- Do not buffer outgoing messages in the executor. The CEC layer contains the
  only transmit FIFO, with eight normal slots per CEC wire and one extra slot
  for a release. At nine messages, further release submissions are dropped.
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
and failed-key no-op handling under IPC pressure. The sole ninth CEC slot is
explicitly permitted only for an executor-selected release; ordinary messages
cannot use it. Once all nine slots are occupied, drop further releases without
retaining or retrying them when capacity opens. Pass generic overflow permission
to the CEC layer; it must not inspect opcodes to choose admission policy.

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
