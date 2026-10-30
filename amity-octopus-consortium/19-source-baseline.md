# Authoritative local behavior and comparison sources

The authoritative behavior baseline is local Amity at `98e64a6`, plus the user's explicit amendments in [AGENTS.md](../AGENTS.md). The earlier design pin `d50af8026938797fc0820d0107f4c4e064c2d2bc` is historical source evidence only where behavior is unchanged. Local `main` branch position alone does not supersede the recorded merge baseline.

## Sources to compare before changes

- `components/amity_hub/hub.cpp` and `include/amity/hub.hpp`: activity fields, selective power, source routing/switch fallback, received frames, metadata, held recipients and audio/playback state.
- `components/amity_hub/hub_service.cpp`: owner-task input/RX/setup/discovery sequencing, original allocation/back-first PA selection, asynchronous CEC submission and completion handling.
- `components/ble_remote`, `main/swipe_sequence.hpp` and original controller input code: HID/Siri recognition, macros, input gate, gesture sequence and cancellation.
- `components/cec`: mechanical transmit admission, wire protocol/timing, attempts/results and the eight-slot FIFO.
- Baseline UI sources and the approved presentation baseline `aa9395aaecdc861e1d4cce040079a5ac6121e67d`: original workflow plus explicitly approved visual changes.

Use `git show 98e64a6:<path>` and the relevant baseline tests when comparing changed code. Do not copy an older algorithm merely because its design document contains a detailed proposal.

## Necessary distributed adaptation

Save a qualified owner/adapter/side/OSD selector instead of treating names as globally unique. Resolve the owner at the controller and equipment at that owner; deliver original typed intents/state in causal order. Keep authentication and boot/input fences so stale work cannot reach another endpoint. Keep exact admitted held recipients for cleanup. These changes must preserve the baseline's CEC frames, order, attempts, admission, input lifecycle and AVR/switch behavior. Preserve physical-address selection and discovery handling subject to the explicit scan amendment below.

## Explicit scan/cache amendment — 29 October 2026

The user changed scan eligibility: enumerate only at boot, on an explicit UI scan request, or when actual communication needs an uncached recipient. Lookup enumeration has a 120-second cooldown independently per owner/adapter/CEC side. Boot/UI scans bypass eligibility and refresh the affected side timestamps. There is no periodic enumeration or scan merely from reconnect, activity edits, successful logical-address claiming or cooldown expiry. Expiry does not replay failed work.

Activity configuration changes must not disconnect Amities or discard native HDMI device records, active discovery progress or remote observations, including unreferenced adapters. These are explicit changes to the baseline trigger/invalidation rules, not permission to alter allocation, physical-address choice, CEC frames/budgets, response windows or activity/key decisions. [Discovery](05-discovery-and-topology.md) owns the detailed contract.

The user then explicitly required the single access path in Python Amity's `hdmi.py`, `ControllerImpl.get_device`: check the device cache, rescan on a miss subject to its two-minute throttle, then check that same cache again. All callers use that path, including source comparisons and status handlers. Do not add cache-only variants, separate cache-availability preflight APIs or passive-handler exceptions. Preserve nonblocking owner progress through opaque typed continuations; finish the same original operation after lookup without requiring another input event. Qualified side isolation and authenticated ownership remain necessary, while cache mechanics stay inside the resolver.

The subsequent **29 October 2026 activity-sequencing amendment** requires original Python `ControllerImpl.set_activity`/`fix_current_activity`/`force_standby` order, with `cec.Adapter.transmit` awaiting completion. Each Amity advances its activity sequence one original lookup/command step at a time; another Start/Standby cannot overtake a paused step. Different Amities may run independently. The user's key clarification requires ignored presses to be dropped before pressed-state registration, with an unregistered release a no-op and no ignored-press lifecycle or replay. Preserve the existing startup/accepted-selection `wait_for_release` physical-input gate, including all-buttons-up/one-second clearing, then normal independent multikey streams. This explicitly changes earlier eager role pre-resolution and overlapping activity execution; other baseline decisions, frames and attempt budgets remain unchanged. See [activity execution](07-activity-planning-and-execution.md).

The removed graph model added persistent enabled and route/capability/resource approvals that blocked original AVR/switch activities. It is not part of the baseline and is not required by transport. Migration, UI or tests must not retain that alternative policy.

## Evidence limits

Compare local and remote execution transcripts, including missing equipment and return, not just stored fields or Ready. Current firmware/build identity and software checks are recorded in [VALIDATION.md](VALIDATION.md) and `IMPLEMENTATION_STATUS.json`; older graph-model evidence is historical. Real CEC timing, AV results, combined hardware load and ecosystem retention require physical evidence beyond host tests.
