# Preserve the original local CEC behavior

**27 October 2026 user amendment.** The distributed layer delivers original intents to the selected owner. It does not compile a different CEC policy from persisted topology, capability or route approvals. This document owns local execution parity; [addressing](03-domain-model-and-addressing.md), [discovery](05-discovery-and-topology.md), [activity execution](07-activity-planning-and-execution.md) and [the authoritative amendment](21-original-controller-policy.md) apply.

## 1. Same local algorithm at every adapter

Each configured HDMI adapter runs the original front playback/back TV handling, including allocation, back-first scanning, front physical-address selection, identity announcements, received-frame handling and source isolation. Added Amity devices remain headless and act only for their authenticated original controller. They do not infer an independent activity or promote themselves after link loss.

Moving equipment to another Amity must preserve every CEC frame, wire order, attempt budget, admission decision, repeat/release decision and existing timing. Transport and compatibility labels cannot select different algorithms. Preserve the baseline's existing reaction to received frames and discovery; do not replace it with a new mandatory verified-path policy.

## 2. Original activity operations

Use the original Hub decisions for selective power changes, same-target repair, standby, source input selection, explicit switch-input fallback, audio status and vendor quirks. The owner obtains current LA/PA/type/vendor from ordinary local lookup and translates the original operation there.

A separate AVR/audio target is an ordinary qualified equipment selection. A source that cannot be found can still use the original explicit switch-device/input fallback. Missing one recipient does not authorize an all-or-nothing topology preflight that prevents the rest of the original algorithm from proceeding. Preserve baseline deduplication when one recipient fills several activity roles.

Do not require a stored receiver profile, capability approval, confirmed cable graph, proxy-resource reservation or physical-address evidence record before performing an operation supported by the original local algorithm. Do not construct a new opcode sequence merely because work is distributed.

## 3. Original input and gestures

Only the original controller accepts the sole remote and home/browser input. It keeps the original input gate, Power behavior, Siri macros, gesture recognition and generated press/release ordering. Deliver resulting button state changes and typed local intents to the owning executor. Do not send periodic presses or per-button renewal messages.

Each key continues independently after its own successful completion while it remains down. New keys and other CEC commands submit immediately; another key's outstanding ACK/completion does not block them. A failed key press ends that key; unchanged down reports or authority refresh cannot restart it. A matching later up is a no-op.

## 4. End-of-stream release and the only transmit FIFO

Aggregate pressed keys by physical wire, initiator and destination. `User Control Released` has no key operand. Ending one key must not release another active key at that recipient. Final up submits the recipient's release immediately behind admitted presses, without waiting for first edge, ACK or completion. Last-key failure may also end the stream. Preserve exact safe cleanup state independently of the ended key token.

The executor validates/translates and submits directly. CEC contains the only outgoing FIFO: eight queued-plus-active messages per wire, including releases. No extra release slot, repeat backlog or executor retry queue exists. Drop full-FIFO submissions, log every dropped TX including each member of a rejected atomic batch, and do not retain a dropped release for later capacity. State/completion trackers and bounded IPC are not outgoing frame buffers.

CEC mechanically queues and transmits exactly the frames and attempt budgets supplied. It must not inspect key opcodes to choose policy, repeat/release behavior, admission priority or retry budgets. Every key press has one wire attempt. Other budgets remain those selected by the original executor.

## 5. Transport fences

Authenticate the original controller and selected owner, reject stale boot/configuration/input work and preserve causal ordering. These are message-delivery protections. They must not create a second normal-operation planner, persistent activity availability or new CEC timing. Exact matching cleanup cannot be redirected to equipment resolved later under the same name.

## 6. Validation

Compare baseline and scoped-owner transcripts for TV/source power, separate AVR audio, explicit input selection, source absence, quirks, received frames, same-target repair, independent held keys, finite taps, failed presses, FIFO saturation and releases. Host evidence must assert frames/order/attempts/admission, not only Ready or storage equality. Physical CEC captures remain necessary for timing qualification.
