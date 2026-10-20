# Example fixtures

These files are synthetic fixtures for agents and tests. They are **not directly importable firmware configuration** and do not describe a measured installation. Hardware capabilities and physical addresses are explicit assumptions that must be verified in a real system.

## TV and projector scenario

[tv-projector-system.json](tv-projector-system.json) matches the worked topology in [document 02](../02-consortium-architecture.md): a rack octopus has three populated tentacles, and a room octopus has two. These five tentacles together have ten CEC ports, spread across seven electrical domains in this example.

| Octopus / tentacle | Physical HDMI link | CEC arrangement |
|---|---|---|
| Rack / 0 | Rack streamer → receiver input 1 | Isolated source on back; playback proxy on receiver domain at front. |
| Rack / 1 | Receiver output A → TV input 1 | Designated virtual TV on receiver domain at back; playback proxy on TV domain at front. |
| Rack / 2 | Receiver output B → projector input 1 | Silent/monitor-only receiver output at back; playback proxy on projector domain at front. |
| Room / 0 | TV-side console → TV input 2 | Isolated source on back; second playback proxy on TV domain at front. |
| Room / 1 | Projector-side streamer → projector input 2 | Isolated source on back; second playback proxy on projector domain at front. |

Display-facing domains use proxy LAs 4 and 8; their real display occupies LA0. This example does not depend on five simultaneous playback identities on one bus. If receiver output B shares the receiver's CEC bus in a different real installation, its back port belongs to that shared domain and remains monitor-only while the existing designated TV controller owns LA0. That is a different observed topology and requires a revised domain configuration.

[component-bindings.json](component-bindings.json) records twelve capability bindings. A single native source has separate power and navigation bindings to the same actual endpoint. A receiver-output proxy represents the selected rack source with its own sink-facing LA/PA. The source's actual isolated-side identity and its proxy identity are never treated as interchangeable wire addresses.

Each configured CEC component stores `qualified_names` with consortium/octopus/tentacle/side/OSD-name. Native runtime bindings trace their resolution through `device_name`; proxy bindings omit it. The rack and projector-side streamers both report `Apple TV` in this synthetic fixture, but their qualified attachment prefixes distinguish them. User labels such as “Rack streamer” are presentation text, not the matching OSD name. Activities use internal ComponentId references to those name-bearing records.

The fixture assumes a receiver that can show the same selected input on both outputs. It rejects independent sources through that shared selector. Direct-TV and direct-projector sources have only their actual physical paths; no return-audio or ARC/eARC capability is inferred.

## Wire fixtures

| File | What to inspect |
|---|---|
| [qualified-device-names.json](qualified-device-names.json) | Persisted OSD selectors, including identical Apple TV names on different attachments. No logical addresses. |
| [qualified-addresses.json](qualified-addresses.json) | Resolved runtime addresses only: LA4 repeats on distinct source domains; LA0 repeats on distinct display domains. |
| [name-reresolution.json](name-reresolution.json) | Same configured Apple TV moves LA4 to LA8 while another appliance occupies LA4; selector/ComponentId/config revision stay fixed, binding revision advances, old work is invalid. |
| [operation-ensure-power.json](operation-ensure-power.json) | Coordinator requests discrete power-on from the room octopus through a native source binding. |
| [operation-activate-proxy.json](operation-activate-proxy.json) | The same source is represented by a different, locally owned playback proxy on the TV domain. |
| [operation-press-key.json](operation-press-key.json) | A navigation down state targets the native source binding with an exact token; `repeat` selects owner-local held repetition or a single press/release. No per-button lease is used. |

The control-operation lease and realtime-input lease IDs differ intentionally. These IDs are illustrative references; the files contain no actual handshake or permission grant. To execute a fixture in a simulator, create matching authenticated state, semantic configuration 7, the referenced domain/binding revisions, synchronized BootIds and a receiver-issued lease of the required class. A real endpoint must reject it otherwise.

Configuration revision 7 and presentation revision 2 are independent. Editing a user-facing label increments presentation revision only; changing a configured selector, route or binding policy requires semantic activation and a new config revision. Ordinary unambiguous LA re-resolution changes runtime binding revisions, without changing configuration or requiring a new activity selection. Coordinator/actuator synchronization and stale-work fencing still apply. Protocol envelopes use the semantic revision.

Run [validate_contracts.py](../validate_contracts.py) from the package root to check structural schemas and selected relationships. The companion [validation record](../VALIDATION.md) records what was checked when this proposal was prepared; none of these checks qualifies real hardware.
