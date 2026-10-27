# Original controller, headless owners and original algorithms

This document records the user's authoritative role and execution requirements. [AGENTS.md](../AGENTS.md), [addressing](03-domain-model-and-addressing.md), [local execution](07-activity-planning-and-execution.md), [storage](11-config-storage-and-migration.md) and [UI](20-user-interface.md) apply together.

## Original algorithms and functionality amendment — 21 October 2026

The current behavior baseline is `98e64a6` plus explicit user amendments. Preserve original activity/AVR/audio/switch behavior, discovery and physical-address selection, received-frame handling, remote input/macros/swipes, CEC transmission/attempts, independent key lifecycle and original UI workflows. Distribution moves original intents to an owner; it has zero effect on CEC behavior. Older design proposals and tests cannot authorize different normal-operation algorithms or timing.

Each held key continues after its own successful completion while down. Other input/commands submit immediately. A failed key press ends it and later matching up is a no-op. Release is implicit at the end of the recipient's entire press stream, aggregated per physical wire/initiator/destination, with exact cleanup state retained separately. CEC has the only outgoing FIFO: eight total queued-plus-active messages per wire, releases included. Full-FIFO release stays dropped; log every full-FIFO TX drop. Bounded causal IPC is allowed; hidden outgoing/retry queues are not. [CEC contract](06-cec-roles-and-message-policy.md).

Keep startup/accepted-selection wait-for-release, its one-second fallback and swallowed holds; rejected selection and long-Power repair do not arm it. Keep Siri-only fixed Power/selector macros and unchanged swipe recognition, timing, generated events and cancellation. No per-button renewal or periodic transmitted press stream is added.

## Qualified lookup amendment — 27 October 2026

The configured recipient is **Amity device ID / HDMI adapter / CEC side / OSD name**. The original controller resolves the owner; that owner resolves its local adapter/side/name and performs original handling. Canonical schema 3 stores user intent and selectors without persistent enabled, route/binding/domain/resource/capability approval or PA evidence. Missing equipment stays saved and may return without Edit/Save/Enable/Approve. Separate AVR audio, explicit switch inputs, source-absence fallback and original quirks remain normal local behavior.

Keep durable identity, trust, original/member authority, configuration/reset journals and hardware settings for their own purposes. Runtime discovery, key/completion state and stale-message fences remain transient. No prepared execution plan, durable current-activity resume ledger, profile approval or new normal-operation cadence is added.

The failed migration retained six activity records but made four AVR/switch activities non-executable. Storage equality and Ready were insufficient evidence. Compare actual local/remote execution before claiming parity, and never ask the user to reconfigure existing activities to work around added distributed policy.

## Fixed original controller

One firmware serves both roles. A fresh device has no Wi-Fi/UI; Matter BLE enrollment establishes the original controller and authenticated Amity BLE enrollment establishes a headless added device. Commit the durable role before original-only services or adoption acknowledgment. A committed Matter fabric, disarmed fail-safe and checked persistent connected Wi-Fi, or proven retained original authority, establish original role; a portable document, uncommitted fabric, discovery order or merely receiving Wi-Fi does not.

The first-setup device stays the original controller, sole remote owner, browser/admin host, Matter/HomeKit integration gateway and membership/configuration authority. Its stable ID is explicit, never inferred from hostname, array order, uptime or peer epoch. Reboot, last-fabric removal, rename/reorder, ordinary restore or another screen cannot change it. Ambiguous identity/authority remains passive for recovery rather than guessed.

Added devices host no UI, login, status/recovery page, home integration or physical remote input path. They cannot accept delegated remote ownership, become input gateways, promote/elect themselves or offer automatic failover. Their authenticated management and owner-local execution wait for the original controller. Explicit physical/USB bootstrap/recovery remains separate from hosted UI.

## One remote and original user workflows

The sole supported remote pairs only with the original controller. Preserve its existing Pair/Unpair/status workflow and bond. Selecting another activity/screen does not move pairing or reinterpret a gesture. Browser, Matter and HomeKit controls use the same original activity algorithms. Existing component/activity IDs, labels, order and external slots remain stable.

Keep the approved tab order **Activities, Remote, Matter, HomeKit, Amity devices, Advanced**. Preserve original form/workflow behavior and approved visual refinements, canonical owning-hostname/HDMI-adapter context, one device-management tab and the narrow per-device settings exception. No equipment wizard, Enable/Approve-route control, remote-owner mapping or primary-zone chooser is added. Internal octopus/tentacle/consortium terms remain outside user-facing UI/documentation.

## Settings, removal and manual recovery

Keep stable per-device adapter settings within the advertised one/four-adapter profile. Show saved default, original detected and selected front PA separately. Actual presence/boot/settings-generation checks protect an explicit selected-device atomic save/restart; they do not approve activity routes. CEC silence is not physical presence evidence.

Explicit Remove revokes the selected responding member, drains/fences current work, commits its established reset journal and schedules only that target's restart after response attempt. Offline/unconfirmed removal requires the existing at-least-ten-second physical reset instructions. Generic loss, retirement or setup cancellation does not reset anything. Origin/Add authorization remains retained; operational trust/network/roles are reset-owned.

If the main fails, isolate it, Matter-enroll replacement hardware as a new main and use existing Advanced upload/restore. Preserve portable IDs, original fields/selectors/order/slots/quirks and explicitly map replacement owners; drop obsolete schema-2 executable approvals, old addresses and activity-resume state. Do not disable restored activities pending graph repair. Pair the sole remote and home integrations again; never clone old private keys, pairing stores, network/admin credentials or controller authority.

Factory-reset each old headless member with the existing ≥10-second hold, then use its three-to-below-ten-second Add window and retained Add code to re-enroll with fresh trust. A returning old main stays isolated until explicitly reset/reclaimed. There is no live transfer, member promotion or automatic failover. [Recovery](13-faults-removal-and-recovery.md).

## Acceptance

Verify role-before-service boot, durable immutable original identity, no member UI/input/integration APIs, sole remote ownership, original local/remote activity/gesture/key parity, settings target-only restart, explicit removal/reset distinction and manual recovery without trust cloning. Keep software evidence separate from real radio/CEC/AV/pairing qualification. [Validation](15-validation-and-release-gates.md).
