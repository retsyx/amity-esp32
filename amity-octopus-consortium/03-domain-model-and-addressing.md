# Equipment addressing and saved intent

**27 October 2026 user amendment.** This document owns the qualified lookup model. It supersedes the former persistent binding, electrical-domain, route, capability and resource approval model. [Original behavior](19-source-baseline.md), [execution](07-activity-planning-and-execution.md), [storage](11-config-storage-and-migration.md) and [AGENTS.md](../AGENTS.md) apply together.

## 1. Configured equipment selector

A saved equipment selection is:

`Amity device ID / HDMI adapter / CEC side / OSD name`

The wire/storage spelling remains `octopus_id`, `tentacle_id`, `side` and `osd_name`, scoped by the installation's `consortium_id`. These are internal field names; the browser uses Amity device hostnames and **HDMI adapter N**. A hostname is presentation and network metadata, not device identity. Adapter numbering remains stable when adapters are absent. Each adapter has independent `front` and `back` wires; that distinction is part of the selector.

A component may have a stable `ComponentId` and user label so activities can retain their references across edits. Its single configured qualified OSD selector identifies the actual recipient. The serialized `qualified_names` array contains at most one selector; it is not a list of alternative routes. A retained unresolved legacy placeholder can have no representable selector and must not be silently matched by label. Labels, array order, IP addresses, CEC logical addresses and previous discovery results MUST NOT replace that selector.

```json
{
  "consortium_id": "10000000000000000000000000000001",
  "octopus_id": "20000000000000000000000000000002",
  "tentacle_id": 0,
  "side": "back",
  "osd_name": "Receiver"
}
```

OSD text uses the original shared CEC decoder and matching rules. Duplicate names on different owners, adapters or sides are separate equipment. A missing name remains a valid saved selection. Within the selected wire, preserve the original name/physical-address cache replacement and lookup behavior. Do not add a complete-scan uniqueness proof, freshness threshold or availability latch. Lookup scope never broadens to another owner, adapter or side.

## 2. Lookup and runtime addresses

The original controller resolves the stable owning device through authenticated membership. That owner resolves its configured adapter and CEC side, then resolves the OSD name using its ordinary local discovery state. The result contains the current CEC logical address and observed device metadata. There is no persistent decision that the component or its activity is executable.

A resolved endpoint has the same scope plus `logical_address`. Logical address, physical address, type, vendor, observed power/audio state and current reachability are runtime data. A logical-address change does not rewrite an activity. A later operation uses the original discovery/cache behavior. An admitted held key retains its exact recipient for repetition and final cleanup, as in local Amity; a late release cannot be redirected to a new endpoint.

## 3. Saved activity intent

Save the original activity choices: name, display, source, audio, optional switch device and switch input. Stable activity/component IDs, labels, activity order, home-app slots/aliases are ordinary configuration. Existing zone/presentation references may organize those choices without changing the local execution algorithm.

Do not store activity `enabled`, `route_id`, executable route steps, binding approvals, confirmed domains, capability approvals, signal graphs, resource allocations or physical-address evidence as permission to use those choices. The absence of equipment changes the outcome of current lookup; it does not change saved intent.

An AVR is equipment addressed by its selector. Separate audio and explicit switch inputs use the same local algorithms as before. They do not need a new receiver-control profile or a preapproved source-to-screen graph.

## 4. State that remains necessary

Durable device identity, original-controller authority, membership trust, credentials, configuration revisions/journals, reset journals and hardware/adapter settings remain necessary for their existing purposes. They do not approve an HDMI path.

Runtime state includes discovery caches, current activity/input generation, exact held recipients, CEC completions, ordered IPC and authenticated transport replay/stale-work fences. Keep only state needed by the original algorithms or message authorization. Do not add a durable current-activity resume journal or restore live addresses, holds or grants after boot.

## 5. Acceptance

- The same activity and observations produce equivalent local and remote CEC frames, order, attempt budgets and key lifecycle.
- Temporarily absent owner/adapter/name can return without changing or enabling the activity.
- Name resolution never crosses owner, adapter or side; renaming an Amity does not change equipment identity.
- Existing AVR/audio/switch and power behavior remains available without topology approval.
- Saved configuration contains user intent and selectors, with no executable graph or activity availability bit.
