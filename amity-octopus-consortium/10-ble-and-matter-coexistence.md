# Shared BLE, Matter and setup ownership

The 28 October 2026 user amendment requires automatic Matter setup at eligible zero-fabric boot. The selector/owner-delivery model otherwise preserves radio, Wi-Fi and remote algorithms. Preserve the existing single NimBLE host, CHIP Wi-Fi owner, setup arbiter and physical authorization. [Membership](09-membership-security-and-enrollment.md), [role policy](21-original-controller-policy.md) and [source baseline](19-source-baseline.md) apply.

## One shared runtime

Matter, the sole controller remote and the Amity setup service share the existing host. Added devices retain the substrate needed for authenticated setup/member communication but cannot expose a user-control integration or pair a remote. Do not start a second protocomm/NimBLE host, reinitialize Wi-Fi for enrollment or destroy the host after commissioning.

Register the extra GATT service through the pinned CHIP extra-services hook before Matter starts. Definitions and callback context outlive asynchronous host startup. GATT callbacks perform bounded validation/copying and hand work to workers; crypto, flash and network changes do not run inside callbacks. Preserve bounded fragmentation/reassembly, minimum-MTU support and connection/generation correlation.

Use the exact SDK pins in `SDK_VERSIONS.txt` and inspect their actual service-registration behavior before changing hooks. A similarly named API in another SDK release is not evidence for this build.

## Role before services

A freshly prepared board has no Wi-Fi credentials and no browser UI. Matter BLE setup establishes the original controller; authenticated Amity BLE setup establishes a headless added device. Resolve durable role before exposing any original-only service. Last-fabric removal and reboot do not turn an established device into an unclaimed candidate.

The application must automatically open the bounded Matter commissioning window at boot when the current fabric count is zero and the board is either unclaimed or the proven original main with retained authority. Factory-reset and still-unclaimed reboots open it again. This is a permanent feature, requires no setup-button GPIO, and must not be replaced by a required button press without a new explicit user request. Route it through the shared setup arbiter after stored role/identity is resolved.

Keep global SDK no-fabric shortcuts from bypassing that role check: a headless member is already commissioned through Amity even with no Matter fabric. Prepared, retired and recovery roles do not automatically open Matter setup. A proven original main whose last fabric was removed opens it at the next boot while retaining main authority/UI and explicit Matter-tab pairing controls. Saved activities/configuration and previous commissioning history do not gate opening; a current fabric does. An unconfigured board may confirm firmware health before human setup completes; lack of enrollment alone is not a boot failure.

## Physical gestures

Preserve the existing debounced single-action button interpretation:

| Gesture | Result when eligible |
|---|---|
| Release before three seconds | Optional initial-setup reopen on an eligible unclaimed board. Boot opening is automatic. |
| Release after at least three and before ten seconds | Bounded candidate window for the main’s Amity devices → Add scan. |
| Hold reaches ten seconds | Existing durable factory-reset path. Neither shorter setup action fires first. |

Configured/prepared/member/retired/recovery states keep their existing eligibility restrictions. A raw absence of Wi-Fi/fabrics does not erase a committed role. Automatic initial Matter setup works without a button; explicit candidate enrollment on buttonless profiles retains its USB-assisted physical path. Indicators and printed instructions must match actual hardware; do not claim an unconfigured detector or button exists.

## Arbiter and callbacks

Keep the existing one-workflow radio reservation, local deadline, in-memory generation, connection handles and cancellation lifecycle. Matter setup, remote pairing and Amity candidate/main admission work cannot replace one another through late callbacks. Existing remote reconnect/input continues under its original policy and measured resource budget.

To support Add while boot Matter setup is idle, an explicit candidate or main Add request may close that window. Active Matter BLE/PASE or an armed fail-safe blocks the handoff. Wait for actual GAP advertising shutdown and owner cleanup within the existing shared five-second budget; preserve exact generation fences so delayed close/cleanup callbacks cannot release the new owner.

Do not add normal-operation timeouts, repeat cadence or radio recovery merely to accommodate distribution. Enrollment/setup deadlines remain their existing separate contracts. No periodic per-button lease renewal or transmitted press stream is added. The owner repeats a held key using its original completion-paced local handling.

## Wi-Fi and persistence

Admission transfers supported credentials only after the selected encrypted session is established and only when needed. The normal profile uses ephemeral key exchange; the optional setup-code profile uses the printed Matter code through PAKE. Both ends require the same profile, without fallback; this exchange neither opens Matter commissioning nor creates a fabric. Stage/commit through the existing CHIP Wi-Fi owner, preserve an already networked candidate and distinguish network success from committed member authority. Preserve original main/Matter fail-safe handling. Hostname selection is authenticated metadata and never device identity.

## Evidence

Test service registration and role resolution before the actual boot commissioning call. Cover unclaimed/factory-reset reboots, no-button operation and a proven main after last-fabric removal; current fabrics and headless/prepared/retired/recovery roles must prevent automatic opening. Keep exact gesture, long-hold exclusivity, stale callback, fragmentation/minimum-MTU and cancellation checks. Physical qualification must cover real Matter setup, remote gestures/reconnect and custom GATT under CEC/radio load; host tests alone do not establish that interoperability.
