# Amity device ownership and transport

This package documents a thin distributed layer around original local Amity. The configured equipment address is **Amity device ID / HDMI adapter / CEC side / OSD name**. The original controller locates the owner; that owner resolves its local equipment and runs the same local algorithms.

The 27 October 2026 user instruction removes the earlier persistent executable-topology/planner design. Canonical schema 3 stores original activity choices and qualified selectors. There is no activity enabled flag, approved route, resource/capability graph or persisted PA evidence required for normal operation. Missing equipment stays saved and uses ordinary runtime lookup. AVR/audio, explicit receiver input, quirks, discovery, gestures and CEC behavior remain those of baseline `98e64a6` plus the user's explicit amendments. Read [AGENTS.md](../AGENTS.md) first.

## Documents

| Document | Owns |
|---|---|
| [03 — Addressing](03-domain-model-and-addressing.md) | Saved qualified OSD selectors and separate runtime CEC addresses. |
| [04 — Hardware/runtime](04-hardware-and-cec-runtime.md) | Stable adapters, original per-adapter runtime and electrical qualification. |
| [05 — Discovery](05-discovery-and-topology.md) | Owner-local lookup and original physical-address selection. |
| [06 — CEC behavior](06-cec-roles-and-message-policy.md) | Original local execution, keys/releases and the sole FIFO8. |
| [07 — Activity handling](07-activity-planning-and-execution.md) | Original activity decisions with owner delivery. |
| [08 — Transport](08-consortium-transport-and-routing.md) | Authenticated typed delivery, ordering and stale-work rejection. |
| [09 — Membership](09-membership-security-and-enrollment.md) | Physical bootstrap, identity, trust and enrollment. |
| [10 — Shared radio/setup](10-ble-and-matter-coexistence.md) | Matter/NimBLE/Wi-Fi task ownership and setup gestures. |
| [11 — Storage/migration](11-config-storage-and-migration.md) | Schema-3 user intent, migration, backups and journals. |
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
- Each adapter has independent front/back wires and runs the original allocation/discovery/PA algorithm.
- Owner/adapter/OSD absence changes current lookup outcomes, never a persistent activity availability bit.
- Keep identity, original-controller/member authority, trust and configuration/reset journals for their separate purposes. Runtime keys, completions and stale-message fences are not persistent route approvals.
- Each key repeats after its own successful completion; failures end it. Transport state changes, not periodic presses or button renewals.
- CEC mechanically transmits supplied frames/budgets through eight total active/queued slots per wire. Full-FIFO releases stay dropped and every full-FIFO TX drop is logged.
- Preserve sole original UI/remote/integrations, headless members, no automatic failover, stable hostname/adapter context and approved per-device settings.
- A build, Ready or retained JSON is insufficient migration evidence; compare actual behavior with the baseline, including AVR/switch and missing equipment return.
