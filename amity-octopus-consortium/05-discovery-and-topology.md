# Owner-local discovery and equipment lookup

**27 October 2026 user amendment.** Discovery supplies the original local Amity algorithms with runtime equipment data. It does not build a persistent approved topology. [Addressing](03-domain-model-and-addressing.md), [CEC behavior](06-cec-roles-and-message-policy.md), [execution](07-activity-planning-and-execution.md) and [source baseline](19-source-baseline.md) define the contract.

## 1. Lookup ownership

The coordinator looks up an authenticated Amity device ID. That owner looks up its stable HDMI adapter and isolated front/back CEC side, then the exact configured OSD name in its local discovery state. The owner resolves the current logical address, physical address, type and vendor when the original algorithm needs them.

Local and remote equipment use the same name-resolution rules. The transport does not interpret a missing name as permission to select similar equipment, move to another adapter, change the activity or permanently disable it. A later lookup can succeed when the selected equipment returns.

## 2. Scan triggers and retained discovery

**29 October 2026 user amendment.** HDMI discovery runs for exactly three reasons: boot, an explicit scan requested in the UI, or an actual attempt to communicate with equipment that is missing from its owner's cache. Activity edits and configuration application, member reconnection, and successful logical-address claiming do not independently request a device scan.

A communication lookup may enumerate only the recipient's isolated CEC side. Each owner/adapter/side has its own 120-second cooldown, measured from its most recent enumeration. Boot and explicit UI scans bypass eligibility and refresh the timestamps of the sides they enumerate. A missing lookup during the cooldown returns unresolved; expiry alone does not schedule a scan, retain a retry, or replay failed work. Requests may reuse an already-admitted scan of their relevant side under the existing one-time deferred handling; this does not queue another scan or a retry at cooldown expiry. A later actual communication attempt may request another scan. Logical-address claiming remains separate electrical address maintenance.

Compare `components/amity_hub/hub.cpp`, `hub_service.cpp` and their tests with baseline `98e64a6` for unaffected behavior. Preserve asynchronous polling/reply handling, known-device commands, discovery-triggered handling, input repair, attempt budgets and response windows. Do not add setup wizards, preflight scans or a required manually confirmed physical graph. A side-only lookup scan retains the other side's cached devices; the existing cache stays usable while its replacement scan is in progress.

Discovery results and availability may be reported to the original controller for its existing UI. Reporting does not transfer CEC policy or address selection to that controller. Discovery data remains scoped to the actual owner/adapter/side. Saving activities preserves native device records, discovery progress and remote adapter observations, including adapters that no saved activity references. A cached observation cannot become a persistent equipment selector or authorization for stale work.

### One device-access path

**29 October 2026 clarification.** Every caller uses the same owner-local `find_device`/`get_device` equivalent. It checks the cache, requests a scan subject to the same side-local cooldown on a miss, and reads that same cache again when lookup completes. Callers do not inspect cache presence or choose a cache-only, passive, forced, or availability-preflight variant. This includes activity power/input, keys, source comparisons, power-status replies and audio-status handling. The reference is `ControllerImpl.get_device` in the original Python Amity `hdmi.py`.

The owner loop remains nonblocking. It may suspend the original typed operation behind an opaque pending lookup and finish it once the relevant scan completes; the caller does not need a second incoming event. A continuation retains its next operation step, so already attempted side effects are not replayed: source-theft suppression attempts Standby once before a cold switch lookup resumes only the original input handling. Received-operation continuations use the existing bounded owner IPC reservations, with activity, owner boot, adapter identity, binding and lifetime fences. They are not outgoing CEC frames, replay queues or an alternative device-access path. Source ownership/scope validation and authentication happen before resolution; an assigned source on another owner/adapter is not a local OSD name to scan for. Exact admitted key recipients remain retained for safe cleanup.

### Announcements are independent of activities

**29 October 2026 announcement amendment.** A valid player announcement adds or updates its device record before activity response policy, whether there are no saved activities, the system is in standby, or another source is selected. Cache ingestion never filters by activity/component references or an in-flight selection. A valid broadcast Active Source supplies its observed logical/physical address; Report Physical Address, directed Set OSD Name and broadcast Vendor ID also populate an unknown peer. Query that peer's remaining identity using the ordinary 83/46/8c requests. Active Source alone does not establish device type; use its actual identity report rather than infer type from a logical-address pool.

Those metadata requests target the announced peer. Updating a record by its physical wire/address does not enumerate unrelated addresses or add another name-resolution policy. Enumeration still follows only boot, explicit UI requests and the shared resolver's throttled cache-miss lookup. Reject unregistered source 15 and malformed announcement framing; Active Source also requires its broadcast destination and a valid physical address.

An early or unselected source may correctly receive Standby under the current activity, including while a future activity is still in flight. That response does not remove its cache record. The normal activity sequence later issues the appropriate power command when it reaches that selected device. Do not add pending-source immunity or configuration/announcement coordination to avoid this accepted race.

## 3. Each adapter uses the original address algorithm

Each configured HDMI adapter is an independent instance of the original front/back pair. Preserve baseline address allocation, polls, identity announcements, attempts and timing. On boot and explicit full scans, scan the back first and determine that adapter's front physical address from the numerically lowest usable reported back-device address, otherwise its saved default. Reject the same malformed/root/unknown values as the baseline. Then allocate/announce and scan the front as the original code does. A side-only communication lookup enumerates only its requested side.

The selected front address remains fixed for that boot under the original rules. Later scans update metadata; they do not create a new physical-address approval or force the user to reapprove activities. A saved fallback is the original startup setting, not evidence in an executable route graph. Adapter 1's result must not affect another adapter.

## 4. Missing and detachable adapters

Hardware configuration determines which stable adapter slots exist. Preserve the approved per-device settings and configured-slot checks. There are no separate adapter-presence inputs. Ordinary owner-local lookup and CEC transmission determine results from actual device responses and ACKs.

An unavailable selected adapter or OSD name uses ordinary local failure/discovery handling. Save the user's selections unchanged. Returning equipment does not start a missed activity, replay relative input or revive a failed held key. It also does not require editing an `enabled` flag, rebuilding bindings or revalidating a topology. A previously admitted key's cleanup remains tied to the original wire, initiator and destination.

## 5. Configuration and migration

Configuration validation checks selector syntax, device scope, adapter capacity and original activity fields. It does not require equipment to answer during save or migration. Preserve names that are offline at migration time, including AVR audio and explicit receiver input choices. Runtime logical/physical addresses, discovered types/vendors and observations are not portable saved state.

## 6. Evidence

Required parity cases include absent equipment and its return, repeated OSD names on different adapters/owners/sides, address changes during held input, all baseline front-address inference/fallback cases, simultaneous independent adapters and normal scan/command progress while a TX is outstanding. Hardware timing and radio qualification remain separate from host tests. See [validation](15-validation-and-release-gates.md).

`tests/cec_scan_policy_test.cpp` exercises the real HubService worker for boot and UI scans, independent side and adapter cooldowns, unresolved lookups without deferred retries, unscanned-side cache retention, and absence of configuration/rebind/claim-triggered enumeration. Cold audio-status, power-status and source-comparison lookups finish their original received event after the same cache is refilled, while unrelated RX progresses. Stale activity/context and unauthorized work are rejected, and pending received events share the existing 32 ordinary reservations with a bounded lifetime. `tests/owned_hub_test.cpp` checks that activity edits retain native devices, in-progress discovery and remote observations on selected and unselected adapters.

`tests/device_announcement_test.cpp` exercises announcement cache insertion and metadata completion through the actual owner worker without saved/active device selection, with valid Standby response and retained records. It also checks unselected equipment, peer-only identity queries, repeated announcements and malformed/unregistered rejection.
