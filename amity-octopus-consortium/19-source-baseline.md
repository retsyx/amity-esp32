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

Save a qualified owner/adapter/side/OSD selector instead of treating names as globally unique. Resolve the owner at the controller and equipment at that owner; deliver original typed intents/state in causal order. Keep authentication and boot/input fences so stale work cannot reach another endpoint. Keep exact admitted held recipients for cleanup. These changes must preserve the baseline's CEC frames, order, attempts, admission, input lifecycle, discovery/PA and AVR/switch behavior.

The removed graph model added persistent enabled and route/capability/resource approvals that blocked original AVR/switch activities. It is not part of the baseline and is not required by transport. Migration, UI or tests must not retain that alternative policy.

## Evidence limits

Compare local and remote execution transcripts, including missing equipment and return, not just stored fields or Ready. Current firmware/build identity and software checks are recorded in [VALIDATION.md](VALIDATION.md) and `IMPLEMENTATION_STATUS.json`; older graph-model evidence is historical. Real CEC timing, AV results, combined hardware load and ecosystem retention require physical evidence beyond host tests.
