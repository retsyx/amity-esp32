# Original Amity handling and qualified owner dispatch

`amity::Hub` remains the behavior source for activities, separate AVR/audio, source routing and explicit switch-input fallback, received frames, discovery and held keys. Compare baseline `98e64a6` and [AGENTS.md](../../AGENTS.md) before changing those algorithms. Distribution adds scoped recipients and typed delivery; it does not provide a second CEC policy.

The configured equipment address is Amity device ID / HDMI adapter / CEC side / OSD name, scoped to the installation. `SystemControl` carries original activity choices and stable presentation/slot identity. The main delivers shared activity selections; each Amity runs the original local sequence for its own equipment and resolves its own adapter/side/OSD devices. Each activity step waits for lookup and CEC transmission completion before the next step. Each Amity runs independently; old-only Standby commands precede new-only power-on commands within that Amity, without waiting for other owners. Activity startup never waits for a device application response or reported power state. Missing equipment stays configured and uses ordinary lookup rather than an enabled/approved-route gate.

Each configured adapter is an independent original front/back pair. `HubService` owns local input/RX/setup/discovery and CEC completion work. Preserve the original allocation candidates, asynchronous scans, lowest usable back-device PA or that adapter's saved fallback, fixed-for-boot front PA, announcement order and original received-frame/configuration effects. An adapter's default/observations never determine another adapter's identity.

The sole original controller hosts UI, home integrations and the remote. Added devices remain headless and accept only authenticated original-controller delivery. No planner/resource/domain/profile graph, prepared global execution plan or durable current-activity/resume ledger is part of normal execution. Canonical schema 3 stores original activity fields, qualified selectors, labels/order/slots. [Addressing](../../amity-octopus-consortium/03-domain-model-and-addressing.md) and [execution](../../amity-octopus-consortium/07-activity-planning-and-execution.md).

## Keys, submissions and cleanup

The main sends raw key-down/key-up edges to the selected Amity and remembers only that owner's index for each pressed key. Each owner has the full activity description, chooses audio or source, and uses the shared resolver for each down, local repeat and release. No per-key resolved CEC recipient is retained. Finite taps are atomic down/up edge pairs; held-button CEC press transmissions are generated only by the owner.

One owner-level table preserves independent logical keys, translated held Play/Pause opcode, failed state and local completion progress. Each still-down key may press again after its own successful transmission; new keys submit without waiting for another key's ACK or capacity. Failure ends that key until up, and unchanged down reports cannot restart it. Resolve aggregate release against the current recipient of remaining active source/audio keys. The operand-free release is submitted behind admitted presses without waiting for their completion; a dropped full-FIFO release is not retained.

Accepting an activity transition stops old holds and fences pending old user-key lookups and queued presses before the first wire edge. An already-transmitting frame finishes. Resolve release cleanup under the old current activity before this owner's ordered initialization advances. Old up events and completions cannot affect a later hold, and setup-period input is discarded through the original gate. Other owners remain independent.

CEC contains the only outgoing FIFO: eight total queued-plus-active messages per wire, releases included. The executor submits directly and does not keep a retry/repeat backlog. Full-FIFO submissions are dropped and every dropped TX is logged, including members of rejected atomic batches. A dropped release is consumed, not retried when capacity opens. CEC mechanically transmits supplied frames/attempt budgets; it does not inspect opcodes to choose policy.

Bounded causal task IPC and completion bookkeeping remain necessary. They must preserve down/up ordering and failed-key no-op handling under pressure, without becoming outgoing frame storage. Authentication/current boot/configuration/input fences reject stale distributed work without changing normal CEC timing or original task ownership.

## Task memory

The 0.1.122 four-owner physical scan exposed DMA pressure: the main's largest free DMA block was 272 bytes when AES/TLS needed a 512-byte allocation. The 0.1.123 run moved the existing per-adapter key state to PSRAM, verified all sixteen adapter tables externally allocated across four DUTs, and passed all twelve physical cases with clean teardown. Before-test main internal free space increased from 24,179 to 35,431 bytes; the largest internal block increased from 9,728 to 13,312 bytes. Evidence is `test-results/remote-keys-final-2026-11-01/memory-placement-123.json` and `all123-verification.json`.

The subsequent user-directed simplification removes the replicated per-adapter intent/auth/key tables. There is one bounded 256-entry lifecycle table per owner and one byte per raw key for main-side owner routing. Task-only state is allocated after [main.cpp](../../main/main.cpp) enables the existing external-memory policy. ISR-visible generation counters, CEC engines and the owner stack remain internal. A third generic frame-start guard enforces the user-input transition cutoff without opcode policy, timing changes or a second outgoing queue. Current native and physical outcomes are recorded with the corresponding final-run evidence; the earlier failed runs remain intact.

## Original input

Keep the shared original controller input algorithms: startup and accepted-selection wait-for-release (including same target), all-up clearing and one-second fallback, swallowed holds not replayed, rejected selection/long-Power repair not arming the gate. Keep Siri-only fixed Power+selector macros and exact swipe/tap recognition, acceleration, timing, generated sequences and cancellation.

Devices exchange resulting state changes/intents, not periodic presses or button renewals. The emitting owner performs repetition and aggregate stream release using the original algorithm.

## Evidence

[Control-surface evidence](CONTROL_SURFACE_EVIDENCE.md) distinguishes current checks from historical graph-model validation. Compare baseline and scoped local/remote frame transcripts, including AVR/audio/switch, unavailable equipment return, original discovery/PA and FIFO/key behavior. A Ready device or retained JSON is insufficient to prove parity. Physical radio/CEC/AV and four-adapter timing qualification remain separate.
