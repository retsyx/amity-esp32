# Owner-local discovery and equipment lookup

**27 October 2026 user amendment.** Discovery supplies the original local Amity algorithms with runtime equipment data. It does not build a persistent approved topology. [Addressing](03-domain-model-and-addressing.md), [CEC behavior](06-cec-roles-and-message-policy.md), [execution](07-activity-planning-and-execution.md) and [source baseline](19-source-baseline.md) define the contract.

## 1. Lookup ownership

The coordinator looks up an authenticated Amity device ID. That owner looks up its stable HDMI adapter and isolated front/back CEC side, then the exact configured OSD name in its local discovery state. The owner resolves the current logical address, physical address, type and vendor when the original algorithm needs them.

Local and remote equipment use the same name-resolution rules. The transport does not interpret a missing name as permission to select similar equipment, move to another adapter, change the activity or permanently disable it. A later lookup can succeed when the selected equipment returns.

## 2. Preserve original discovery behavior

Compare `components/amity_hub/hub.cpp`, `hub_service.cpp` and their tests with baseline `98e64a6` before changing discovery. Preserve the original scan requests, asynchronous polling/reply handling, known-device command behavior, discovery-triggered handling, input repair and timeouts. Do not introduce additional freshness periods, setup wizards, preflight scans or a required manually confirmed physical graph.

Discovery results and availability may be reported to the original controller for its existing UI. Reporting does not transfer CEC policy or address selection to that controller. Discovery data remains scoped to the actual owner/adapter/side. A cached observation cannot become a persistent equipment selector or authorization for stale work.

## 3. Each adapter uses the original address algorithm

Each configured HDMI adapter is an independent instance of the original front/back pair. Preserve baseline address allocation, polls, identity announcements, attempts and timing. Scan the back first and determine that adapter's front physical address from the numerically lowest usable reported back-device address, otherwise its saved default. Reject the same malformed/root/unknown values as the baseline. Then allocate/announce and scan the front as the original code does.

The selected front address remains fixed for that boot under the original rules. Later scans update metadata; they do not create a new physical-address approval or force the user to reapprove activities. A saved fallback is the original startup setting, not evidence in an executable route graph. Adapter 1's result must not affect another adapter.

## 4. Missing and detachable adapters

Hardware configuration determines which stable adapter slots exist. Preserve the approved per-device settings and actual presence handling. Missing detection hardware means attachment is unknown; CEC silence is not proof that a detachable module is physically absent.

An unavailable selected adapter or OSD name uses ordinary local failure/discovery handling. Save the user's selections unchanged. Returning equipment does not start a missed activity, replay relative input or revive a failed held key. It also does not require editing an `enabled` flag, rebuilding bindings or revalidating a topology. A previously admitted key's cleanup remains tied to the original wire, initiator and destination.

## 5. Configuration and migration

Configuration validation checks selector syntax, device scope, adapter capacity and original activity fields. It does not require equipment to answer during save or migration. Preserve names that are offline at migration time, including AVR audio and explicit receiver input choices. Runtime logical/physical addresses, discovered types/vendors and observations are not portable saved state.

## 6. Evidence

Required parity cases include absent equipment and its return, repeated OSD names on different adapters/owners/sides, address changes during held input, all baseline front-address inference/fallback cases, simultaneous independent adapters and normal scan/command progress while a TX is outstanding. Hardware timing and radio qualification remain separate from host tests. See [validation](15-validation-and-release-gates.md).
