# Synthetic selector and owner-intent examples

All IDs are synthetic. These examples illustrate the current contracts; they contain no credentials, network configuration or real hardware authority.

- [activity-configuration.json](activity-configuration.json) is canonical schema-3 saved intent: a TV and separate AVR on one Amity, a source on another, and a Wii activity retaining explicit AVR input 6 even when that source is absent. There are no activity enabled flags or executable topology approvals.
- [qualified-device-names.json](qualified-device-names.json) keeps exact owner/adapter/side/OSD scope. Identical OSD names on different scopes are distinct equipment.
- [qualified-addresses.json](qualified-addresses.json) contains separate runtime logical-address locators; those are not portable saved selectors.
- [owner-state.json](owner-state.json) reports runtime owner/adapter discovery and audio state, separate from saved configuration.
- [name-reresolution.json](name-reresolution.json) shows a current logical-address change without editing the saved selector; the next key operation resolves the current address.
- [activity-selection.json](activity-selection.json) delivers the selected owner-local activity to a headless Amity. That owner independently runs its original power/input recipe; no cross-owner CEC step or result is transported.
- [operation-press-key.json](operation-press-key.json) delivers a raw button edge to the owner on the ordered authenticated connection, without an adapter, side or OSD selector. The owner repeats through its original successful-completion pacing; no periodic press or per-button renewal is transported.

The former binding/resource/physical-route graph fixtures and proxy activation example are removed. Current lookup and original algorithms determine execution. Fixture acceptance does not establish physical AV success, radio interoperability or production security.
