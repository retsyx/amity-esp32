# Qualified equipment and original owner intents

This component defines stable IDs, qualified OSD selectors, separate runtime CEC endpoints and typed original operations. [Addressing](../../amity-octopus-consortium/03-domain-model-and-addressing.md), [execution](../../amity-octopus-consortium/07-activity-planning-and-execution.md) and [AGENTS.md](../../AGENTS.md) own the model.

A configured recipient is installation scope plus Amity device ID, stable HDMI adapter, CEC side and OSD name. Its current logical address, PA, type, vendor and availability come from owner-local runtime discovery. Labels, IPs and prior logical addresses are not saved selectors. Missing equipment stays selected and can return without an activity enable/edit operation.

`owned_intent.hpp` carries original operations such as power, source/switch input, announcement, audio query and key state, plus exact transient generation/sequence/hold and authorization context. Port-only original operations may omit OSD text. Owner snapshots report ordinary adapter/device observations; they do not approve a persisted topology.

Original `Hub`/owner handling decides CEC frames, ordering, attempts, repeats/releases and failure behavior. The distributed layer delivers typed intent to the selected owner and rejects stale/unauthorized work. No persistent enabled flag, executable route graph, resource/capability/domain approval, prepared plan or durable current-activity resume ledger is part of this core.

Preserve strong nonzero stable IDs, canonical bounded numeric codecs, exact selector scope and the shared OSD decoder's octet rules. Keep runtime held recipients/completions and message authorization fences needed to prevent stale work reaching another endpoint; these are distinct from a stored permission to use an activity. Physical CEC protocol/FIFO remains in the CEC layer.

Software transcript tests must compare baseline local and scoped local/remote handling for AVR/audio/switch, absent equipment return, address changes during holds and hard FIFO8. Physical electrical/AV/radio behavior requires separate qualification.
