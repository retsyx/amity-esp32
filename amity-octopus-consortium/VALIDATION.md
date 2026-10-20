# Validation of this documentation package

**Package:** Amity Octopus and Consortium, proposed v0.1 with the OSD-name and guided-setup amendments. **Prepared and amended:** 20 October 2026. **Source baseline:** `d50af8026938797fc0820d0107f4c4e064c2d2bc`.

## Scope

This record concerns the documentation, schemas and synthetic fixtures in this package. It is separate from the firmware/hardware release plan in [document 15](15-validation-and-release-gates.md). No feature implementation, firmware build, flashing, hardware timing measurement, BLE pairing, Matter induction or end-to-end AV test was performed.

The source review used an isolated reference worktree at the stated commit. It examined existing hardware/CEC, configuration, remote, Matter, HomeKit, provisioning, storage and test paths and the two source issues. Implementation-oriented cross-review checked the architecture, addressing, BLE/security, configuration, routing, lifecycle and test contracts against each other.

## Automated package checks

The supplied [validator](validate_contracts.py) checks Draft 2020-12 schema validity, positive and negative configured-name/address/message vectors, selected topology/binding/operation relationships, the synthetic LA re-resolution fixture, local Markdown targets, reference labels and balanced fences. It checks that native runtime bindings refer to a configured name at the same attachment and that proxy bindings do not masquerade as native selectors. With an optional reference checkout it checks that the cited immutable repository paths exist. It uses a local schema registry and does not fetch remote references.

The final automated package check passed using Python 3.12 and jsonschema 4.26.0. Results are recorded below. These checks do not establish wire timing, electrical domain truth, cryptographic implementation correctness, full portable-configuration validation or runtime authority enforcement.

| Check | Result |
|---|---|
| JSON schemas checked against Draft 2020-12 | 5 passed |
| Positive structural vectors | 45 accepted |
| Negative/malformed structural vectors | 38 rejected |
| Component binding relationship checks | 12 passed |
| Persisted qualified-name records | 6 passed, including identical OSD text on distinct attachments |
| Native binding-to-configured-name relationships | 7 passed |
| LA re-resolution example | Passed: unchanged selector/component/config revision, LA4 to LA8, advanced binding revision, old LA occupied by a different name, stale queued target distinguished |
| Operation-to-binding relationship checks | 3 passed |
| Populated CEC ports in example topology | 10, each assigned to exactly one example domain |
| Markdown files checked | 24 |
| Local linked targets checked | 327, all present |
| Distinct pinned repository paths checked | 50, all present |
| Firmware or hardware tests run | None |

## Review results

The architecture and focused implementation documents received independent cross-review. The final review incorporated these corrections: alternate source representations may share one exclusively allocated proxy identity; cold boot does not restore stale group role claims; barriers close old-generation admission before draining; receiver output mode can require fresh PA evidence and a new plan; source wake is explicit; input freshness is checked on both network hops; presentation edits have a separate revision; and restart resumption requires fresh intent.

The TV/projector fixture received a separate CEC/physical semantics review. Its per-attachment PA evidence, native/proxy target distinction, local executing roles and domain address capacity are internally consistent with its declared assumptions.

The OSD-name amendment received a focused consistency review across architecture, discovery, configuration, transport and lifecycle contracts. Persisted qualified names now end in `osd_name`; logical addresses remain in separate runtime endpoints. Unambiguous LA changes update all derived runtime bindings automatically without activity/configuration edits or confirmation, while old work remains fenced. The fixture checks establish the consistency of these proposed contracts and examples, not that a firmware resolver, binding-update protocol or stale-frame rejection has been implemented. Those behaviors have explicit acceptance cases in document 15.

The [guided-setup amendment](20-user-experience-and-guided-setup.md) received independent HDMI/topology and enrollment/security review. Corrections distinguish board location from screen destination, Amity connectors from appliance inputs, automatically disambiguate colliding activity titles, and constrain diagnostic effects to committed valid bindings. First setup uses the initialized singleton and explicit eligible physical entry; busy Matter workflows retain non-preemption. The related storage, UI, work-package and traceability documents now cover bounded resumable choices, partial draft activation, Save/Try separation and UX-01 through UX-16. These are specified acceptance cases; no interface was implemented and no novice usability study was performed.

## Remaining implementation evidence

Every hardware/SDK/capacity gate in [document 17](17-decisions-and-open-questions.md) remains open unless explicitly resolved by future implementation evidence. In particular, ten-line timing, safe pin selection, sink proxy capacity, physical-address applicability, Security2 client interoperability, external Matter commissioning arbitration, zero-fabric Wi-Fi persistence, operational TLS verification and combined memory/storage budgets need real implementation and qualification.

The synthetic topology declares its capability assumptions. A schema-valid example must still fail at a real actuator unless it has matching authenticated membership, current authority, binding/domain/configuration state, correct capability and a valid receiver-issued lease.
