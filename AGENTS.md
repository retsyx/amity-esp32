# Preserve original Amity behavior

The current `main` implementation is the authoritative behavior baseline.
This merge incorporates `main` at `98e64a6`; its independent key lifecycle,
asynchronous CEC submission, input gate and Siri macros supersede conflicting
older baseline rules. Preserve that behavior unless the user explicitly
requests a change. This rule applies to implementation, design documents,
tests and future maintenance.

The distributed path is layered on the same code as `main`. It only moves
messages to the owning Amity; it must have zero effect on CEC behavior. Local
and remote equipment must receive equivalent frames, order, attempt budgets,
key lifecycle, repeat/release decisions and queue-admission behavior for the
same input. Native/compatibility labels identify transport plumbing, never
alternative CEC policy.

The consortium is an additional ownership and transport layer around those
algorithms. It is not permission to redesign activity planning,
AVR/audio/switch handling, discovery, physical-address selection, remote input,
CEC transmission, retries, held-button repetition, or existing UI workflows.
Keep existing behavior for both local and remotely owned equipment. Make only
the adaptations required to identify the owning device, deliver the original
intent, and fence stale or unauthorized distributed work. Do not introduce new
normal-operation timing, timeouts, cadence, or recovery behavior without an
explicit user request.

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
