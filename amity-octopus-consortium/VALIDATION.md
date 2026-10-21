# Validation of this documentation package

The behavior baseline is current `main` (`98e64a6` for this merge), with identical CEC behavior for local and remote ownership. Historical interface evidence at `d50af8026938797fc0820d0107f4c4e064c2d2bc` remains relevant where unchanged, with the current [original-controller policy](21-original-controller-policy.md) and [baseline interface](20-user-interface.md). One firmware supports one main and headless actuators. The sole remote and all user-facing services remain on the main. Manual replacement-main backup recovery establishes fresh authority; no automatic election, live transfer or pairing cloning exists.

The 21 October 2026 amendment makes preservation of original algorithms/functionality normative unless the user explicitly requests a change, including Siri swipe recognition, timing and generated event sequences. Held-button contracts now carry state changes only, with the original completion-paced CEC repeat loop at the output owner. Per-button renewal/lease requirements are superseded; first-delivery freshness and current general link/plan authority remain separate. [Repository-wide instruction](../AGENTS.md).

Subsequent explicit user changes require one attempt per key-press frame, ended key state and repetition after TX failure with later button-up a no-op, implicit global release at recipient stream end only when all its keys cease being pressed (including last-key failure), no executor outgoing buffer, and a mechanical CEC layer with the only FIFO with eight normal slots plus one release-only overflow slot per wire. The executor owns button/message choices. Package checks validate contract consistency; actual direct-admission, atomic-batch, failure/up and single-FIFO software evidence is recorded separately in implementation status.

## Package checks

Run `.cache/contracts-python/bin/python amity-octopus-consortium/validate_contracts.py` from the repository root, or a Python environment with `jsonschema`4.x and `referencing`. The validator performs no network, device or firmware action. It checks:

- Current manifest file coverage, sizes and SHA256 digests, excluding the manifest itself.
- Six local Draft2020-12 schemas with a local reference registry.
- Positive and malformed address/name/binding/operation/hostname-metadata/per-adapter-settings/removal-reset vectors, including rejection of removed input/transfer/remote-management/hold-renew kinds, obsolete per-button lease fields and obsolete generic retirement wire phases.
- Scoped native/proxy binding relationships and a synthetic LA re-resolution case without persisted LA selectors.
- Local Markdown targets, reference labels and balanced fences. Optional `--reference-repo` checks historical baseline citation paths.

This record covers package consistency only. Compiler, host/browser, exact image and historical hardware evidence live in [implementation status](../IMPLEMENTATION_STATUS.json); the [release gates](15-validation-and-release-gates.md) remain separate. Schema acceptance does not establish executable authority, electrical correctness, cryptography, actual pairing retention or AV picture/audio.

## Current structural checks

| Check | Result |
|---|---|
| JSON schemas |6 checked|
| Positive vectors |63 accepted|
| Negative vectors |94 rejected|
| Binding relationships |12 checked|
| Native configured-name relationships |7 checked|
| Operation relationships |3 checked|
| Synthetic populated CEC ports |10|
| Runtime LA re-resolution fixture |Passed|
| Firmware/hardware tests run by this validator |None|

The manifest and validator output are the authoritative package counts. Removed guided-wizard/draft, multi-remote/member-input and controller-transfer files are not retained as implementation contracts or positive fixtures. The internal core, scoped discovery, multi-CEC runtime, guarded semantic planning, authenticated enrollment, configuration synchronization and updates remain required.
