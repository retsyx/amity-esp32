# Amity device ownership and transport

This package documents a thin distributed layer around original local Amity. The configured equipment address is **Amity device ID / HDMI adapter / CEC side / OSD name**. The original controller locates the owner; that owner resolves its local equipment and runs the same local algorithms.

The 27 October 2026 user instruction removes the earlier persistent executable-topology/planner design. Canonical schema 3 stores original activity choices and qualified selectors. There is no activity enabled flag, approved route, resource/capability graph or persisted PA evidence required for normal operation. Missing equipment stays saved and uses ordinary runtime lookup. AVR/audio, explicit receiver input, discovery, gestures and CEC behavior remain those of baseline `98e64a6` plus the user's explicit amendments. The [29 October scan/cache amendment](05-discovery-and-topology.md#2-scan-triggers-and-retained-discovery) explicitly changes scan eligibility and preserves caches across activity edits. Read [AGENTS.md](../AGENTS.md) first.

## Documents

| Document | Owns |
|---|---|
| [03 — Addressing](03-domain-model-and-addressing.md) | Saved qualified OSD selectors and separate runtime CEC addresses. |
| [04 — Hardware/runtime](04-hardware-and-cec-runtime.md) | Stable adapters, original per-adapter runtime and electrical qualification. |
| [05 — Discovery](05-discovery-and-topology.md) | Owner-local lookup, scan triggers/cooldowns, retained caches and original physical-address selection. |
| [06 — CEC behavior](06-cec-roles-and-message-policy.md) | Original local execution, keys/releases and the sole FIFO8. |
| [07 — Activity handling](07-activity-planning-and-execution.md) | Original activity decisions with owner delivery. |
| [08 — Transport](08-consortium-transport-and-routing.md) | Authenticated typed delivery, ordering and stale-work rejection. |
| [09 — Membership](09-membership-security-and-enrollment.md) | Physical bootstrap, identity, trust and enrollment. |
| [10 — Shared radio/setup](10-ble-and-matter-coexistence.md) | Matter/NimBLE/Wi-Fi task ownership and setup gestures. |
| [11 — Configuration storage](11-configuration-storage.md) | Current-format user intent, strict version checks, backups and journals. |
| [12 — Controls/admin](12-control-surfaces-and-admin.md) | Original controls, integration identity and management APIs. |
| [13 — Recovery](13-faults-removal-and-recovery.md) | Loss, explicit removal/reset and manual replacement. |
| [14 — Updates](14-ota-and-compatibility.md) | Exact release/image verification and compatible target updates. |
| [15 — Validation](15-validation-and-release-gates.md) | Baseline local/remote parity and remaining physical gates. |
| [19 — Source baseline](19-source-baseline.md) | Authoritative local source and evidence limits. |
| [20 — Interface](20-user-interface.md) | Original forms/tabs and approved visible exceptions. |
| [21 — Original controller](21-original-controller-policy.md) | Sole controller/remote/UI, headless members and manual recovery. |

[Machine-readable contracts](schemas/README.md), [synthetic examples](examples/README.md) and [recorded validation](VALIDATION.md) are part of this package. `MANIFEST.json` covers its current Markdown, JSON and Python files. Old planner graph schemas/examples are removed rather than retained as an active alternative design.

## Invariants

- Original CEC behavior is identical for locally and remotely owned equipment. Distribution is ownership and delivery, not a new planning policy.
- Each adapter has independent front/back wires and preserves original allocation, polling, reply handling and physical-address selection, subject to the explicit scan amendment.
- HDMI scans run only at boot, on an explicit UI request, or for an actual communication cache miss. Lookup scans have an independent 120-second cooldown per owner/adapter/side; boot/UI scans bypass it and refresh those timestamps. Expiry alone does not request a scan or replay failed work.
- Activity edits do not disconnect Amities, clear HDMI device caches or prune observations based on saved references. Reconnects and periodic timers do not independently request device enumeration.
- Valid announcements add/update device records before current-activity policy, independently of saved/selected equipment. An early or unselected player may correctly receive Standby and remain cached; normal activity power follows when its sequence reaches that device.
- Every device caller uses one cache-first owner-local resolver: cache, throttled scan on miss, then the same cache again. No cache-only handler variants or cache-availability preflights exist; an opaque pending lookup resumes the original operation without blocking the owner loop.
- Start/Standby follows the original Python recipient/lookup/command-completion order on each Amity, with no later activity overtaking a paused sequence. Different Amities and the owner loop may advance in parallel. Ignore activity-transition key presses before registering them; their releases are no-ops, with no ignored-input lifecycle or replay. Preserve the existing physical-input gate and normal independent multikey behavior after it clears. The continuation keeps only original-function state, never outgoing CEC frames or persistent resume data.
- Owner/adapter/OSD absence changes current lookup outcomes, never a persistent activity availability bit.
- Keep identity, original-controller/member authority, trust and configuration/reset journals for their separate purposes. Runtime keys, completions and stale-message fences are not persistent route approvals.
- Each key repeats after its own successful completion; failures end it. Transport state changes, not periodic presses or button renewals.
- CEC mechanically transmits supplied frames/budgets through eight total active/queued slots per wire. Full-FIFO releases stay dropped and every full-FIFO TX drop is logged.
- Preserve sole original UI/remote/integrations, headless members, no automatic failover, stable hostname/adapter context and approved per-device settings.
- A build, Ready or retained JSON is insufficient migration evidence; compare actual behavior with the baseline, including AVR/switch and missing equipment return.
