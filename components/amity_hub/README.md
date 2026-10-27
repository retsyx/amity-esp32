# Original Amity handling and qualified owner dispatch

`amity::Hub` remains the behavior source for activities, separate AVR/audio, source routing and explicit switch-input fallback, vendor quirks, received frames, discovery and held keys. Compare baseline `98e64a6` and [AGENTS.md](../../AGENTS.md) before changing those algorithms. Distribution adds scoped recipients and typed delivery; it does not provide a second CEC policy.

The configured equipment address is Amity device ID / HDMI adapter / CEC side / OSD name, scoped to the installation. `SystemControl` carries original activity choices and stable presentation/slot identity. The controller sequences the original decisions and selects the owner; the owner resolves local adapter/side/OSD equipment and applies the original local operation. Missing equipment stays configured and uses ordinary lookup rather than an enabled/approved-route gate.

Each configured adapter is an independent original front/back pair. `HubService` owns local input/RX/setup/discovery and CEC completion work. Preserve the original allocation candidates, asynchronous scans, lowest usable back-device PA or that adapter's saved fallback, fixed-for-boot front PA, announcement order and original received-frame/configuration effects. An adapter's default/observations never determine another adapter's identity.

The sole original controller hosts UI, home integrations and the remote. Added devices remain headless and accept only authenticated original-controller delivery. No planner/resource/domain/profile graph, prepared global execution plan or durable current-activity/resume ledger is part of normal execution. Canonical schema 3 stores original activity fields, qualified selectors, labels/order/slots and quirks. [Addressing](../../amity-octopus-consortium/03-domain-model-and-addressing.md) and [execution](../../amity-octopus-consortium/07-activity-planning-and-execution.md).

## Keys, submissions and cleanup

Preserve independent logical keys. Each still-down key repeats after its own successful completion. New keys and other commands submit immediately without waiting for another key's ACK or capacity. A failed key press ends it; unchanged down and authority refresh cannot restart it, and matching later up is a no-op. Keep the accepted recipient for a hold even if discovery changes its name's current logical address.

Release is implicit at recipient stream end over all active keys sharing physical wire, initiator and destination. Final up submits release immediately behind admitted presses without waiting for completion or first edge. Last-key failure can also end a stream. Retain exact safe cleanup state independently from the ended token; never release a newly resolved recipient in its place.

CEC contains the only outgoing FIFO: eight total queued-plus-active messages per wire, releases included. The executor submits directly and does not keep a retry/repeat backlog. Full-FIFO submissions are dropped and every dropped TX is logged, including members of rejected atomic batches. A dropped release is consumed, not retried when capacity opens. CEC mechanically transmits supplied frames/attempt budgets; it does not inspect opcodes to choose policy.

Bounded causal task IPC and completion bookkeeping remain necessary. They must preserve down/up ordering and failed-key no-op handling under pressure, without becoming outgoing frame storage. Authentication/current boot/configuration/input fences reject stale distributed work without changing normal CEC timing or original task ownership.

## Original input

Keep the shared original controller input algorithms: startup and accepted-selection wait-for-release (including same target), all-up clearing and one-second fallback, swallowed holds not replayed, rejected selection/long-Power repair not arming the gate. Keep Siri-only fixed Power+selector macros and exact swipe/tap recognition, acceleration, timing, generated sequences and cancellation.

Devices exchange resulting state changes/intents, not periodic presses or button renewals. The emitting owner performs repetition and aggregate stream release using the original algorithm.

## Evidence

[Control-surface evidence](CONTROL_SURFACE_EVIDENCE.md) distinguishes current checks from historical graph-model validation. Compare baseline and scoped local/remote frame transcripts, including AVR/audio/switch, unavailable equipment return, original discovery/PA and FIFO/key behavior. A Ready device or retained JSON is insufficient to prove parity. Physical radio/CEC/AV and four-adapter timing qualification remain separate.
