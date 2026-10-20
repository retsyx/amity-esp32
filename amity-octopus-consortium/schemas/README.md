# Contract schemas

These JSON Schema Draft 2020-12 files describe the proposed v0.1 design. They are an implementation starting point, not schemas accepted by current Amity firmware. Their `https://amity.invalid/...` IDs are identifiers for local registry resolution; no service is deployed at that address.

| File | Coverage |
|---|---|
| [core-types.schema.json](core-types.schema.json) | Nonzero 128-bit IDs, exact uint64 decimal strings, port references, persisted qualified OSD names and separate runtime CEC addresses. |
| [qualified-device-name.schema.json](qualified-device-name.schema.json) | Persisted device selector: consortium, octopus, tentacle, side and exact OSD name. Rejects logical-address substitution. |
| [cec-address.schema.json](cec-address.schema.json) | Resolved runtime equipment/proxy endpoint, including consortium, octopus, tentacle, side and current LA. Rejects OSD-name substitution. |
| [component-binding.schema.json](component-binding.schema.json) | Native/proxy distinction, native configured device_name, runtime endpoint, role, revisions, evidence and proxy resource. |
| [wire-envelope.schema.json](wire-envelope.schema.json) | Closed common envelope, kind registry, and closed payload/argument schemas for the five initial semantic operations. |

The binding schema's `role` is the local executing port's role, not the remote appliance's device type. `proxy_identity_resource_id` identifies one allocated proxy identity; alternative source representations can share it only as described in [the domain model](../03-domain-model-and-addressing.md). A structural schema does not grant the identity or prove its PA.

Native bindings require `device_name` equal to one of their component's persisted `qualified_names` and with the same port prefix as their runtime endpoint. Proxy bindings forbid that field; their allocated identity is not a native discovery selector. An unresolved configured name without any observed endpoint has no runtime binding yet. Historical bindings with non-verified status cannot execute; their cached LA is not a current identity.

The name schema checks nonempty decoded text and its character bound. The shared CEC decoder must additionally validate the original payload's octet budget and encoding; preserve exact name comparison, including spaces, case and delimiter characters. Human-readable slash-separated names are display strings, not an alternative parser syntax. Neither schema can prove that a live appliance reported a name or that its match is unambiguous.

## Explicit limits

The envelope schema does **not** yet specify the payload of every registered message kind. Each implementation package must add the owning payload schema and positive/negative vectors before enabling that kind. The runtime parser must reject unknown payload fields according to that completed contract. Do not treat the schema's generic placeholder for those payloads as permission to accept arbitrary data.

No deployable portable-configuration schema is supplied. [Document 11](../11-config-storage-and-migration.md) defines the required records, revision semantics and migration; W12 must freeze its complete bounded schema as part of implementation. The topology fixture illustrates relationships and is deliberately labeled as a fixture.

JSON Schema cannot establish peer authentication, ownership, live membership, electrical-domain truth, binding freshness, matching destination/target IDs, PA applicability, current execution lease, resource grants or CEC capability. These remain mandatory semantic checks at admission and wire start. A schema-valid packet is not necessarily authorized or executable.

## Validation

From the package root, run:

```sh
python3 -m pip install jsonschema
python3 validate_contracts.py
```

The validator registers every schema locally, checks the schemas themselves, validates positive/negative fixtures and checks selected cross-object relationships. It also checks local document links, Markdown fences and cited baseline paths when `--reference-repo` is supplied. It performs no network request, firmware build or hardware test.

The IDs and setup context in the examples are synthetic. None is a credential or an enrolled authority. Runtime cryptographic enrollment, key proof, leases and timing must be implemented and tested separately.
