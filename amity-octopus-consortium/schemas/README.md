# Contract schemas

These JSON Schema Draft 2020-12 files validate the current design contract and synthetic fixtures. They are not a substitute for the deployed strict codecs and their complete [protocol schema](../../components/consortium/protocol-v1.schema.json). Their `https://amity.invalid/...` IDs are identifiers for local registry resolution; no service is deployed at that address.

| File | Coverage |
|---|---|
| [core-types.schema.json](core-types.schema.json) | Nonzero 128-bit IDs, exact uint64 decimal strings, canonical DNS hostnames, port references, persisted qualified OSD names and separate runtime CEC addresses. |
| [qualified-device-name.schema.json](qualified-device-name.schema.json) | Persisted device selector: consortium, octopus, tentacle, side and exact OSD name. Rejects logical-address substitution. |
| [cec-address.schema.json](cec-address.schema.json) | Resolved runtime equipment/proxy endpoint, including consortium, octopus, tentacle, side and current LA. Rejects OSD-name substitution. |
| [component-binding.schema.json](component-binding.schema.json) | Native/proxy distinction, native configured device_name, runtime endpoint, role, revisions, evidence and proxy resource. |
| [adapter-settings.schema.json](adapter-settings.schema.json) | Closed settings snapshots and write cores/HTTP requests, stable slot labels, canonical counters, PA hierarchy and explicit restart confirmation. |
| [wire-envelope.schema.json](wire-envelope.schema.json) | Closed common envelope, kind registry, and closed payload/argument schemas for the five initial semantic operations negotiated hostname-only device metadata, per-adapter settings and explicit factory-reset removal. |

The binding schema's `role` is the local executing port's role, not the remote appliance's device type. `proxy_identity_resource_id` identifies one allocated proxy identity; alternative source representations can share it only as described in [the domain model](../03-domain-model-and-addressing.md). A structural schema does not grant the identity or prove its PA.

Native bindings require `device_name` equal to one of their component's persisted `qualified_names` and with the same port prefix as their runtime endpoint. Proxy bindings forbid that field; their allocated identity is not a native discovery selector. An unresolved configured name without any observed endpoint has no runtime binding yet. Historical bindings with non-verified status cannot execute; their cached LA is not a current identity.

The name schema checks nonempty decoded text and its character bound. The shared CEC decoder must additionally validate the original payload's octet budget and encoding; preserve exact name comparison, including spaces, case and delimiter characters. Human-readable slash-separated names are display strings, not an alternative parser syntax. Neither schema can prove that a live appliance reported a name or that its match is unambiguous.

## Explicit limits

The envelope schema does **not** yet specify the payload of every registered message kind. Each implementation package must add the owning payload schema and positive/negative vectors before enabling that kind. The runtime parser must reject unknown payload fields according to that completed contract. Do not treat the schema's generic placeholder for those payloads as permission to accept arbitrary data.

The portable-configuration contract and strict codec live in [configuration storage](../../components/config_store/include/system_configuration.hpp), with revision and migration requirements in [document 11](../11-config-storage-and-migration.md). The topology fixture illustrates relationships and is deliberately labeled as a fixture; it is not an accepted portable configuration.

The settings projection cannot prove unique `tentacle_id` values across distinct rows, current boot/revision/generation equality, actual physical presence, owner idleness, durable storage or restart outcome. The runtime codec and owner tests enforce these; a configured detector removal/reinsertion rejects an old dialog even when presence returns to true.

JSON Schema cannot establish peer authentication, ownership, live membership, electrical-domain truth, binding freshness, matching destination/target IDs, PA applicability, current execution lease, resource grants or CEC capability. These remain mandatory semantic checks at admission and wire start. A schema-valid packet is not necessarily authorized or executable.

## Validation

From the package root, run:

```sh
python3 -m pip install jsonschema
python3 validate_contracts.py
```

The validator registers every schema locally, checks the schemas themselves, validates positive/negative fixtures and checks selected cross-object relationships. Hardware capacity comes from the single `AMITY_MAX_TENTACLES` definition in [hardware_limits.hpp](../../components/amity_system/include/amity/hardware_limits.hpp). The validator checks schema bounds, settings sizes/labels, Kconfig connector menus/profile and release-tool limits against it; these static formats cannot drift independently. It also checks local document links, Markdown fences and cited baseline paths when `--reference-repo` is supplied. It performs no network request, firmware build or hardware test.

Add `--hardware-capacity-only` to run the canonical capacity/schema/Kconfig/tooling check using only the Python standard library, before schema meta-validation, manifest and Markdown-link checks. The complete invocation above remains the fixture and document-package check.

The IDs and setup context in the examples are synthetic. None is a credential or an enrolled authority. Runtime cryptographic enrollment, key proof, leases and timing require separate source, host and physical evidence; schema validation establishes none of them.

## Explicit removal reset authorization

`membership_event` and `membership_result` now carry only the closed `factory_reset` phase, exact mutation/digest/membership revision and expected target BootId. Results are `reset_scheduled`, `rejected` or `unknown`; old `retire`/`retired` payloads are rejected. The digest binds the authenticated original main, target/current boots, link, group/epoch/configuration and mutation. These live equalities and negotiated `factory_reset_removal` capability 65536 remain semantic checks, not schema-derived permission. The current known capability mask is 258047, including `owner_repeated_keys` 131072.

## Button state changes and original algorithms

`key_press` carries a key, exact `hold_token` and `repeat` flag. A true flag installs down state at the output's executor and starts completion-paced repetition; false is a finite press/up recipe: submit the press directly, then attempt final release immediately only if the press was admitted and no other key keeps the recipient stream open. It does not require atomic two-frame admission or a first-edge dependency. `hold_release` ends only its matching live state, and is a no-op for a forgotten failed hold. The executor chooses each message and supplies one attempt per key frame. Failed key TX ends that key's pressed state and repetition. The executor derives implicit global release at recipient stream end only when all its keys cease being pressed, including last-key failure; remote-up is a state change, not a direct CEC-release mapping. Only CEC buffers outgoing frames, in its eight-message per-wire FIFO counting queued and active messages together, with no opcode/button policy. Releases have no extra capacity: full-FIFO release admission drops the release permanently, and every TX dropped because the FIFO is full is logged, including every frame of a rejected atomic batch. The old per-button `hold_lease_ms` field and `hold_renew` kind are rejected; numeric slot 4 is retired, never reused. Current link/plan/route/binding authority remains a semantic fence, separate from first-delivery freshness. Positive execution requires negotiated `owner_repeated_keys`; old peers retain only their compatible management/update path until upgraded. [Original-algorithm preservation](../21-original-controller-policy.md#original-algorithms-and-functionality-amendment--21-october-2026), including Siri remote swipe recognition/timing/sequences, and the explicit amendments govern the resulting events.

A `reset_scheduled` result acknowledges a durably written reset journal after actual owner quiescence; it does not prove a completed reboot. Mere roster removal/admission, an offline device, enrollment cancellation or a lost result cannot claim that the member reset. The browser reports local removal separately and requires the physical button for at least 10 seconds when reset is unconfirmed. No automatic destructive replay is permitted. Synthetic vectors establish parser boundaries only; they never reset hardware.

Membership revision zero is a valid canonical uint64 at the wire shape layer, matching the deployed parser. It grants no reset permission: the owner requires a strictly newer membership revision than its current durable revision and rejects zero/stale revisions before quiescence or erasure. Exact identity, expected BootId, phase, digest, deadline and current original-main checks remain unchanged.
