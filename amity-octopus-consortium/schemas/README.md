# Current machine-readable contracts

These closed contracts describe qualified saved intent and owner delivery. Schema IDs are local identifiers, not a deployed service. [Addressing](../03-domain-model-and-addressing.md), [storage](../11-configuration-storage.md) and [transport](../08-consortium-transport-and-routing.md) own the semantics.

| Contract | Scope |
|---|---|
| [core-types.schema.json](core-types.schema.json) | Stable opaque IDs, canonical uint64 strings, adapter/side scope, persisted OSD selectors and separate runtime CEC endpoints. |
| [qualified-device-name.schema.json](qualified-device-name.schema.json) | Saved Amity device ID / HDMI adapter / CEC side / OSD name. |
| [cec-address.schema.json](cec-address.schema.json) | Runtime resolved endpoint; never a saved device selector. |
| [configuration.schema.json](configuration.schema.json) | Canonical schema 3: original activity intent, scoped components, labels/order/slots; no enabled or executable graph approvals. |
| [wire-envelope.schema.json](wire-envelope.schema.json) | Closed current envelope, scoped owner intent/result and supported management payload references. |
| [adapter-settings.schema.json](adapter-settings.schema.json) | Per-device adapter settings, saved defaults and configured-slot, current owner boot, revision and authority checks. |

The production codec and `components/consortium/protocol-v1.schema.json` own exact management payloads and numeric wire kinds. The design envelope references that schema instead of maintaining a divergent management copy. Scoped context retains selected/source-changed plus source, audio, display, switch-device and input choices. A scoped result is either an owner snapshot or a generation/sequence-correlated source-input presence result. Volatile setup-generation/last-sequence/pending-completion and expected-replies-pending metadata preserves the original controller activity-settling latch across owner delivery without adding an owner quiet period; it does not gate other keys/commands, buffer outgoing frames or approve a plan. Scoped intent/result use new slots 41/42 and capability `qualified_lookup` 262144; retired operation/plan/profile/config-replication and relay slots remain retired. Direct admission removes relay kinds 7–10 and capability 2048; numbers are not reassigned.

Configuration `kind` is equipment presentation metadata, not a capability approval. A component has at most one configured qualified selector; an empty retained placeholder is not permission to choose another target. An activity may reference absent equipment. Runtime addresses, observations, active input and completions remain separate from saved intent.

Power intent arguments contain exactly one boolean field, `on`; unknown fields
are rejected. Configuration has no application-defined power payloads or
compatibility decoder for such fields. Peers must implement the same closed
payload contract; mixed older/newer execution formats are unsupported.

JSON Schema checks shape and bounds. The codec/validator must also check installation/owner/adapter scope, unique IDs/selectors, actual references, ordering/slot integrity and byte limits. Authorization, fresh transport context and physical electrical behavior cannot be established by a JSON fixture. Schema validation does not prove firmware behavior or CEC timing.
