// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/system/model.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace amity::system {
// Original Amity actions, before owner-local name resolution and CEC admission.
// These are IPC/transport requests, never an outgoing CEC frame backlog.
enum class OwnedIntentKind {
    power, source_input, switch_input, announce_active_source, query_audio,
    context, standby_wire, discover, activity, key_edges
};
struct KeyEdge { uint8_t key=0; bool down=false; };
struct OwnedIntent {
    OwnedIntentKind kind = OwnedIntentKind::query_audio;
    // key_edges uses only owner/group as local delivery metadata; its wire
    // destination is the authenticated envelope. Other actions select a port.
    QualifiedDeviceName target;
    uint64_t generation = 0;
    // Local IPC metadata. The authenticated envelope carries this on wire.
    uint64_t config_revision = 0;
    // Long-Power repair is one-shot and does not arm the selection input gate.
    bool repair = false;
    // A fresh selection of the current activity repeats its power/input recipe.
    // Its generation still distinguishes it from a recovered duplicate.
    bool reselect = false;
    // Owner-task IPC metadata for its original local recipe, never on wire.
    bool activity_step = false;
    uint8_t input = 0;
    bool on = false;
    std::array<KeyEdge,2> edges{};
    uint8_t edge_count=0;
    // source_input also carries the original display selector so its owner can
    // announce its paired front when the display is on another adapter.
    // Non-emitting context mirrors the original controller's selected activity
    // for follower replies and play/pause state, without replaying its recipe.
    bool selected = false, source_changed = false;
    std::optional<QualifiedDeviceName> source, audio, display, switch_device;
};
// Local owner-task completion, not a wire message or saved execution state.
enum class OwnedContextResult {published,stale,failed};
enum class OwnedAuthority {control,input,cleanup};
struct OwnedAuthorization {
    OctopusId controller_id;
    BootId controller_boot, owner_boot;
    LinkId link_id;
    uint64_t config_revision = 0, deadline_ms = 0;
    OwnedAuthority authority = OwnedAuthority::control;
};
struct ScopedSelector {
    std::string value;
    QualifiedDeviceName target;
};
struct OwnedDispatchResult {
    bool admitted = false;
    std::optional<bool> source_present;
    bool pending = false;
    OwnedDispatchResult(bool accepted=false,std::optional<bool> present={},bool waiting=false)
        : admitted(accepted),source_present(present),pending(waiting) {}
    explicit operator bool()const{return admitted;}
};
struct OwnedDeviceObservation {
    Side side = Side::back;
    std::string osd_name;
    uint8_t logical_address = 15, device_type = 0xff;
    uint16_t physical_address = 0xffff;
    uint32_t vendor_id = 0;
    bool resolved = false;
};
struct OwnedAdapterSnapshot {
    OctopusId owner;
    BootId boot;
    uint8_t adapter = 0, front_address = 15, back_address = 15;
    uint16_t front_physical_address = 0xffff;
    bool ready = false, discovering = false;
    uint32_t discovery_generation = 0;
    bool audio_status_known = false, muted = false;
    uint8_t volume = 127;
    std::vector<OwnedDeviceObservation> devices;
};
} // namespace amity::system
