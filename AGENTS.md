# Amity

## General

- Don't update this file unless explicity asked to do so.

- This project is in early development. There is no need for backward compatibility with prior revisions of the software. This remains true as long as this instruction is in this file. Support only the current saved configuration format. When the software is more mature, this instruction will be replaced with an instruction to ensure backward compatibility between software revisions.

- When working on a feature, don't change ancillary items that are not directly related to the feature. If in doubt, ask.

- Don't babble in documents. Only update documents with information that is pertinent to the long term understanding of Amity's architecture and codebase.

- Identify the actual invariant before “fixing” a race. Not every possible ordering is a bug.

- Do not put internal protocol or build-profile terminology or descriptions into the UI or the top README.md.

## Adapters

- There are no separate inputs that detect whether an HDMI adapter is attached. The absence of an adapter
is implied by the CEC line never going high when idle. An absent adapter produces no CEC responses. Do not add another availability gate based on a sensor, saved selection, cache observation or an assumed physical topology.

## Activities

- **Current means current.** An activity in flight is not active.
- **Discovery is independent of configuration.** Announcements populate the device cache. Editing activities must neither clear it nor disconnect member links.
- **Device lookup has one path.** Every caller uses `find_device()`. Only that function knows about caching and the two-minute scan cooldown.
- **Scans have exactly three triggers:** boot, an explicit UI request, or a required-device cache miss.
- **Activity steps are sequential per Amity.** Different Amities may progress concurrently. User presses during setup are discarded through the input gate.
- **Distribution delivers ownership and messages.** It should not introduce another activity planner or source policy.
- **Tests should expose problems, not conceal them.** Verify fresh physical commands and resulting state without adding workarounds for legitimate timing differences.
- Preserve `wait_for_release` input gate at startup and accepted
  activity selections, including same-target selection, with all-buttons-up
  clearing and the one-second fallback. Swallowed holds must not replay after
  expiry. Rejected selections and long-Power repair do not arm the gate.
- Saving or applying activity configuration must not disconnect Amity devices or
clear their HDMI discovery caches. Keep authenticated member connections and
observed equipment independent of saved activity selections, including adapters
with no selected equipment.
- The current activity is the sole source of truth for source-theft handling.

## HDMI-CEC

- Amities agree on activity start/stop intent. Each independently runs its own ordered device sequence. No cross-Amity CEC waits, step planning, or device synchronization; one owner's blocked job must not delay another owner's activity delivery or execution.
- Within each Amity, each activity initialization action waits for its lookup and CEC transmission before the next action. Send old-only devices' Standby before new-only devices' power-on; preserve shared-device behavior.
- Never wait for reported device state, application replies, or a quiet period to advance an activity.
- The resolver may have multiple pending lookups, but resumes waiters FIFO. Each caller stays paused; cached independent work and the owner task remain responsive.
- The main sends only key-down/key-up state changes and remembers the receiving Amity index. Each owner uses the full current activity and the shared resolver for key actions, including release. Held-button CEC press transmissions are generated locally by the owner.
- When an Amity accepts an activity transition, stop old holds and invalidate old pending user-key lookups and queued user presses before initialization. No further old user press may start; an already-transmitting frame can finish. Old releases/completions cannot affect new holds. Normal key-up still preserves admitted-frame FIFO behavior.
- Every CEC key-press frame uses one wire attempt; there are no key-press retries.
- Any failed key-press transmission ends that key's pressed state and stops its
  repetition. A later matching button-up is a no-op.
- Buttons remain active independently. Releasing Right while Volume Up is held
  ends only Right. Normal button-up neither advances a shared cancellation
  counter nor cancels already admitted CEC frames; those finish in FIFO order.
- CEC release is implicit at the end of the executor's press stream, after one
  or more presses, when no keys remain pressed for that receiving CEC device.
  Remote button-up updates state; it is not a direct key-specific release TX.
  Release has no key operand. Final key-up submits release immediately behind admitted presses, without
  waiting for ACK, completion or first edge. A last-key TX failure may likewise
  cause a final release attempt.
- Do not buffer outgoing messages in the executor. The CEC layer contains the
  only transmit FIFO, with eight slots per CEC wire counting queued and active
  messages together. Full-FIFO submissions are dropped, and every TX dropped because the FIFO is full must be logged.
  The executor validates/translates and submits directly; admission failure is reported without a second frame queue or a hidden repeat backlog.
- CEC is a mechanical transmitter. It queues and sends exactly the frames and
  attempt budgets supplied, performs wire protocol/timing, and reports results.
  It does not interpret key opcodes to choose policy or decide button state,
  repeat, release, or which message to send. Those decisions belong to the
  executor. State and completion tracking are not an outgoing message buffer.
- Valid CEC device announcements populate or update the device cache regardless
  of activity configuration, including standby and devices absent from every
  saved activity. Learn the announcing address and reported information and query
  that peer for missing identity metadata without requiring a full bus scan.
- HDMI device enumeration is permitted for exactly three reasons: board boot,
an explicit scan requested by the user in the UI, or an actual communication
attempt whose target device is missing from the cache. That's it!
- Cache-miss lookup scans have a 120-second cooldown independently for every
Amity / tentacle / CEC side. Scan only the missing recipient's side and preserve
the opposite side's cache. Repeated misses during cooldown remain unresolved;
do not defer or queue another scan for later. Boot and explicit UI scans bypass
that lookup cooldown and refresh the scanned sides' cooldown timestamps.
- Retain HDMI-CEC caches through activity edits and connection/configuration updates.
- All device access goes through one resolver. That function
checks the cache, performs a throttled scan on a miss, then checks the same cache
again. The resolver owns the cache and all lookup/scan/cooldown decisions.
Callers must not know whether a result was cached, inspect cache availability,
preflight whether discovery is needed, or request a lookup scan themselves.
Do not introduce passive-handler, cache-only or cache-bypassing lookup paths.
Boot and explicit UI scan requests may force enumeration; ordinary device
access must always use the shared resolver.
- The owner task must stay responsive while resolution waits for scanning. Resume the original operation after the resolver completes, rather than require a second input event or duplicate already-emitted commands. Do not add an outgoing CEC queue.

## TLS

- TLS connection establishment is independent of application synchronization.
An exchange uses the existing connection to its peer; a missing connection is
created implicitly. Reuse a healthy connection, including while synchronization
is incomplete. Never close it to replace it with another connection merely
because synchronization failed.
- One authenticated, ordered TCP connection per Amity pair. No renewable input/control leases, generic replay IDs, or key-hold tokens.

## Consortium

- An explicit Remove deletes the Amity from saved configuration. Remove its owner record, saved name, adapter settings and equipment, and delete every activity that references that equipment through source, display, audio or switch. Show the affected activity names and warn that confirming removal deletes those
activities. Successful completion leaves no saved owner/equipment selector or ordinary metadata for the removed Amity.
- The accepted workflow is **Amity devices → Add** on the original main, which
scans directly for nearby unclaimed boards. The user holds and releases the
candidate's FW RESET button after at least three and before ten seconds,
chooses the candidate if several are found, enters its name, and adds it.
Keep manual Matter reopening and the ten-second factory reset rule; boot
Matter setup opens automatically as required above.
A committed controller/member cannot become an unclaimed candidate by rebooting.
- Do not introduce or retain a separate Add secret, Add-code provisioning/rotation
workflow or stored bootstrap verifier. Preserve stable origin IDs and the
independent factory Matter/admin credentials.
- Discovery is direct from the main; there is no admission relay through another member.
- There is no automatic failover. There is only one main Amity.

## Matter

- Matter commissioning must open automatically at boot when the current Matter
fabric count is zero and the board is either unclaimed or in factory condition.

## Remotes

- Support fixed Power+Select/Up/Right/Down/Left activity macros only
  for Siri remotes: activities 0–4 for short selector holds and 5–9 at 500 ms.
  Execute once on either macro button's first release, measure duration from
  the selector press, and consume the macro until all physical buttons are up.
  These defaults do not require configuration.

## Functional test framework

- Harness code and bench wiring live in `cec-harness/`. The Python functional
framework MUST interact with DUT state through the same authenticated HTTP
endpoints used by the shipping Amity UI. Do not add firmware endpoints,
test-only commands, authentication bypasses or other DUT control surfaces for
functional tests. Extend the harness around the existing UI capabilities.

- `cec-harness/TEST-HARNESS.md` is the source of truth for physical bench wiring.
Parse its HDMI-CEC mapping and setup/reset tables; do not duplicate GPIO maps or
persist virtual home-theater profiles. Each functional scenario owns its
components and releases their CEC addresses when it ends.