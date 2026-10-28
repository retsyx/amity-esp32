# Exact-image updates and compatibility

Updates preserve the original activity model, device identity, membership and independent pairings/settings. They do not require stored HDMI route approval. [Storage/migration](11-config-storage-and-migration.md), [membership](09-membership-security-and-enrollment.md) and [recovery](13-faults-removal-and-recovery.md) apply.

## Release and target validation

Keep authenticated original-controller administration, exact target selection, bounded image/container parsing, inactive-slot writes, native ESP-IDF verification, boot selection and local health confirmation. Validate project/chip, image size/digest, hardware profile/GPIO mapping, partition layout, release/security profile and supported storage/protocol contracts before activation. An upload/receipt is not a confirmed new boot.

A release manifest binds exact image bytes/ELF identity, firmware and manifest versions, chip/profile/layout, supported protocol/capabilities and readable/writable configuration/storage versions. Preserve source commit and SDK/dependency pins. Production publisher signing uses a separate trusted key and must not be disabled by an admin password or peer command. Development unsigned artifacts remain explicitly development; peer TLS authenticates the device, not its booted software.

## One browser update file

The normal picker accepts `amity-update.bin`. The fixed 32-byte header contains magic `AMITY-UPDATE-V1\n`, bounded big-endian manifest/signature lengths and native-image length. Canonical manifest bytes, optional fixed 64-byte publisher signature and unchanged native image follow. Exact total size, digest, native identity and profile must match. `tools/update_bundle.py` packages existing artifacts; it does not invent trust or alter image bytes.

Only native bytes enter OTA verification. Raw `amity.bin` remains available for USB/debug or an explicitly staged supported update. An older installed release that predates containers must use its actual supported upload format; do not send a new container to a parser that expects the native image at byte zero. Full `amity-provisioned.bin` is for initial USB installation and replaces all settings; it is not an OTA application update.

## Qualified owner-delivery compatibility

Canonical saved configuration is schema 3. Conversion retains user selectors/activity fields and drops obsolete enabled/graph approval data; it does not require equipment to answer. Do not roll back to an image that cannot read the committed storage safely or enforce current original-controller/member authority.

`qualified_lookup` capability 262144 identifies the current typed owner-intent contract. Scoped intent/result use new numeric slots 41/42. Retired operation/plan/profile/config-replication and admission-relay slots remain retired and are never reassigned; relay kinds 7–10 and capability 2048 are not supported. An old peer may remain reachable for compatible authenticated management/update but cannot receive work through a superseded planner/CEC policy. Existing `owner_repeated_keys` semantics require state changes with owner-local repetition, not periodic presses or button renewals.

Reject incompatible hardware/protocol/security combinations explicitly. Compatibility must not silently select an alternate CEC algorithm. A required update is a peer transport compatibility issue, not a persistent disabled activity or approved-route bit.

## Existing 0.1.80 installations

A real schema-3 image declares configuration read formats 1–3 and writer 3. The unchanged NVS journal wrapper declares storage read formats 1–2 and writer 2. Do not conflate these axes or label the new configuration writer as 2 for an older installer.

The installed 0.1.80 verifier requires manifest `config_write == 2`, so it rejects a correctly labeled schema-3 browser/member update. That cohort requires a matching-profile USB firmware upgrade preserving factory/NVS partitions. Use the build's ordinary application/bootloader/partition-table flash, not a full fresh-install provisioned image. Legacy v1/v2 decoding retains original intent/recovery state until confirmed boot migration; it does not retain old graph execution policy. After the cohort runs schema-3 firmware, later compatible OTA uses the true schema-3 container. [Build/upgrade instructions](../BUILDING.md#upgrading-an-existing-0180-installation-to-schema-3).

## Targeted and group updates

The original controller brokers member updates through the authenticated management channel. Keep exact release/job/target/offset correlation, bounded chunks, independent target validation, maintenance admission and actual result reporting. Update added devices before the original controller when required by the existing dependency sequence. No member takes over during the controller's planned outage.

Maintenance state and update journals protect an explicit flash transaction. They are distinct from ordinary activity execution; do not preserve a durable current-activity resume ledger or executable plan. Quiesce/drain the selected owner's current wire/cleanup state under the existing rules, never erase unrelated owners' identities or assume CEC success from an update receipt.

After reboot, verify expected image/ELF/version, new boot identity and protected local health. Reconnect under current trust and transport context. Uncertain installation calls for status inspection, not blindly resending chunks or creating a new relative operation. Preserve factory setup codes, origin identity, Wi-Fi, remote bonds, Matter/HomeKit relationships, admin/TLS identity and user intent on normal OTA.

## Security and recovery evidence

Keep production secure-boot, encrypted-secret storage, release-key provisioning/rotation, anti-rollback and supported recovery checks as explicit physical qualification. Partition names or authenticated version reports do not establish production security. Do not program irreversible counters/eFuses as a routine build or update step.

Test bad/truncated/extra data, mismatched image/manifest/signature/profile, wrong target/offset, duplicate/lost chunk/results, power cuts around write/boot selection, rollback-storage compatibility, exact-image readiness and retained independent credentials. Real multi-device update/radio behavior and ecosystem retention require their own recorded hardware evidence.
