# Shared BLE, Matter and setup ownership

The selector/owner-delivery model does not redesign radio, Wi-Fi or remote algorithms. Preserve the existing single NimBLE host, CHIP Wi-Fi owner, setup arbiter and physical authorization. [Membership](09-membership-security-and-enrollment.md), [role policy](21-original-controller-policy.md) and [source baseline](19-source-baseline.md) apply.

## One shared runtime

Matter, the sole controller remote and the Amity setup service share the existing host. Added devices retain the substrate needed for authenticated setup/member communication but cannot expose a user-control integration or pair a remote. Do not start a second protocomm/NimBLE host, reinitialize Wi-Fi for enrollment or destroy the host after commissioning.

Register the extra GATT service through the pinned CHIP extra-services hook before Matter starts. Definitions and callback context outlive asynchronous host startup. GATT callbacks perform bounded validation/copying and hand work to workers; crypto, flash and network changes do not run inside callbacks. Preserve bounded fragmentation/reassembly, minimum-MTU support and connection/generation correlation.

Use the exact SDK pins in `SDK_VERSIONS.txt` and inspect their actual service-registration behavior before changing hooks. A similarly named API in another SDK release is not evidence for this build.

## Role before services

A freshly prepared board has no Wi-Fi credentials and no browser UI. Matter BLE setup establishes the original controller; authenticated Amity BLE setup establishes a headless added device. Resolve durable role before exposing any original-only service. Last-fabric removal and reboot do not turn an established device into an unclaimed candidate.

Suppress the SDK's independent no-fabric automatic commissioning-window and BLE advertising-autostart paths. Open setup only from the existing eligibility/physical authorization policy. An unconfigured board may confirm local firmware health without having completed human setup; lack of enrollment alone is not a boot failure.

## Physical gestures

Preserve the existing debounced single-action button interpretation:

| Gesture | Result when eligible |
|---|---|
| Release before three seconds | Initial bounded Matter setup for a fresh/unconfigured main. |
| Release after at least three and before ten seconds | Bounded candidate window for the main’s Amity devices → Add scan. |
| Hold reaches ten seconds | Existing durable factory-reset path. Neither shorter setup action fires first. |

Configured/prepared/member/retired/recovery states keep their existing eligibility restrictions. A raw absence of Wi-Fi/fabrics is not authorization to reopen setup. Buttonless profiles retain the explicit USB-assisted physical path. Indicators and printed instructions must match actual hardware; do not claim an unconfigured detector or button exists.

## Arbiter and callbacks

Keep the existing one-workflow radio reservation, local deadline, in-memory generation, connection handles and cancellation lifecycle. Matter setup, remote pairing and Amity candidate/main admission work cannot replace one another through late callbacks. Existing remote reconnect/input continues under its original policy and measured resource budget.

Do not add normal-operation timeouts, repeat cadence or radio recovery merely to accommodate distribution. Enrollment/setup deadlines remain their existing separate contracts. No periodic per-button lease renewal or transmitted press stream is added. The owner repeats a held key using its original completion-paced local handling.

## Wi-Fi and persistence

Admission transfers supported credentials only after the selected encrypted session is established and only when needed. The normal profile uses ephemeral key exchange; the optional setup-code profile uses the printed Matter code through PAKE. Both ends require the same profile, without fallback; this exchange neither opens Matter commissioning nor creates a fabric. Stage/commit through the existing CHIP Wi-Fi owner, preserve an already networked candidate and distinguish network success from committed member authority. Preserve original main/Matter fail-safe handling. Hostname selection is authenticated metadata and never device identity.

## Evidence

Test service registration before host startup, role-before-service ordering, both automatic commissioning paths closed, exact gesture thresholds, long-hold exclusivity, stale callback rejection, partial records/minimum MTU and cancellation. Physical qualification must cover real Matter setup, remote gestures/reconnect and custom GATT under CEC/radio load; host tests alone do not establish that interoperability.
