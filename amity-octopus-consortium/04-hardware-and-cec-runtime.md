# HDMI adapters and the local CEC runtime

This document owns hardware scope and the unchanged local runtime. [Addressing](03-domain-model-and-addressing.md), [discovery](05-discovery-and-topology.md), [CEC behavior](06-cec-roles-and-message-policy.md) and [AGENTS.md](../AGENTS.md) define the operating model.

## Hardware scope

An Amity device supports the adapter capacity advertised by its compiled hardware profile. The existing board has one front/back adapter on GPIO1/GPIO2. The development XIAO ESP32-S3 profile supports up to four optional adapters, using overridable pairs 1/2, 5/6, 7/8 and 9/44. Each adapter has two independent CEC wires. Stable internal slots 0–3 are displayed as HDMI adapter 1–4; absent slots never compress numbering.

The shared limits in `components/amity_system/include/amity/hardware_limits.hpp`, Kconfig, release hardware identity and schemas must agree. Do not infer capacity from discoveries. Unsupported GPIO mappings or allocation errors cannot authorize driving an unintended wire. Keep the existing passive error/admin behavior of the selected profile.

The wires use external pull-ups with internal GPIO pulls disabled. Preserve the existing timer architecture and wire protocol/timing. A profile's GPIO mapping is part of firmware compatibility identity. Four-adapter simultaneous electrical/timing behavior requires its own physical qualification; a passing one-adapter capture is not that evidence.

## One original local runtime per adapter

Each configured adapter uses the same original front playback/back TV behavior and CEC algorithms. The owner task handles input, received frames, allocation/discovery, setup and completions while transmissions remain outstanding. Preserve original logical-address candidates, poll/announcement budgets, back-first scan, lowest usable back-device PA or saved fallback and fixed-for-boot selected front PA.

The distribution layer does not allocate identities from a persistent electrical-domain or proxy-resource graph. It selects an owner and adapter; the owner applies the original local algorithm there. Settings and actual hardware availability are separate from a stored approval to use an activity.

The shared `amity_hub` task has a 12 KiB internal-RAM stack. Coordinator activity
handling and owner-local execution can nest on that task even when equipment is
local. The `0.1.85` Xtensa Standby path retained at least 6,304 bytes before final
wire and runtime overhead, so the former 6 KiB allocation overflowed. This resource
allowance is shared across adapters and adds 6 KiB of internal RAM once. It
changes no CEC decisions, task priority, queue capacity or timing. Validate
boot/heap availability and actual Start/Standby wire effects when changing that
budget; [recorded evidence](15-validation-and-release-gates.md#functional-test-framework).

Synchronous ownership dispatch borrows one `OwnedIntent`: the dispatcher uses
a mutable reference to stamp sequence, setup and key-token fields, while the
facade and transport sender take const references. Queued commands and retained
key state own copies before the caller's request can expire. With the same
Xtensa optimization flags, the known nested Standby call-chain subtotal falls
from 6,112 to 3,968 bytes after this change, excluding final owner-send, CEC and
runtime overhead in both measurements. This is compiler stack-usage evidence,
not a runtime high-water measurement, and does not reduce the 12 KiB allocation
or change CEC behavior, IPC admission, deadlines or task ownership.

## Transmission and queue ownership

CEC mechanically transmits supplied frames and attempt budgets through one FIFO per wire, with eight queued-plus-active messages total. Releases share those eight slots. Full admission drops the submitted frame and logs it; every member of a rejected atomic batch is logged. No reserved release slot, executor outgoing backlog or later retry of a dropped release exists.

State and completion trackers belong to the original executor. Each held key continues after its own successful attempt; failed key TX ends it. New commands submit immediately without waiting for another key's ACK or queue capacity. Bounded task IPC remains appropriate where needed for causal request/state handoff.

## Presence and per-device settings

A separate, electrically qualified presence detector may report attached/absent. Without one, physical presence is unknown; CEC silence and saved selections do not prove detachment. Keep actual presence generations for approved settings/remove-reinsert checks. Do not add detection pulses or pull-downs to a live CEC line.

Each adapter has its own saved default front physical address. The owner uses it in the original startup algorithm; it is not a verified route or a stored PA-evidence grant. Explicit per-device Settings retain fresh owner boot/settings/presence checks, atomic save and selected-device restart after the response attempt. See [UI settings](20-user-interface.md#per-device-hdmi-adapter-settings-amendment--20-october-2026).

## Qualification

Host tests establish addressing, queue, state and transcript behavior. Physical checks must establish each GPIO/wire mapping, external electrical behavior, original CEC timing and ACK behavior, sparse/removable adapter safety, simultaneous adapter load and behavior under Wi-Fi/BLE/HTTPS load. Preserve the distinction between CEC ACK and actual AV picture/audio.
