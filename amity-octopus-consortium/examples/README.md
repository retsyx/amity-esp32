# Synthetic selector and owner-intent examples

All IDs are synthetic. These examples illustrate the current contracts; they contain no credentials, network configuration or real hardware authority.

- [activity-configuration.json](activity-configuration.json) is canonical schema-3 saved intent: a TV and separate AVR on one Amity, a source on another, and a Wii activity retaining explicit AVR input 6 even when that source is absent. There are no activity enabled flags or executable topology approvals.
- [qualified-device-names.json](qualified-device-names.json) keeps exact owner/adapter/side/OSD scope. Identical OSD names on different scopes are distinct equipment.
- [qualified-addresses.json](qualified-addresses.json) contains separate runtime logical-address locators; those are not portable saved selectors.
- [owner-state.json](owner-state.json) reports runtime owner/adapter discovery and audio state, separate from saved configuration.
- [name-reresolution.json](name-reresolution.json) shows a current logical-address change without editing the saved selector or redirecting an already admitted held recipient.
- [operation-ensure-power.json](operation-ensure-power.json) delivers the original power intent to the selected owner, which performs local lookup and original CEC handling.
- [operation-press-key.json](operation-press-key.json) delivers key-down state with an exact hold token. The owner repeats through its original successful-completion pacing; no periodic press or per-button renewal is transported.

The former binding/resource/physical-route graph fixtures and proxy activation example are removed. Current lookup and original algorithms determine execution. Fixture acceptance does not establish physical AV success, radio interoperability or production security.
