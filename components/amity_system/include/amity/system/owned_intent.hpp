// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/system/model.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace amity::system {
// Original Amity actions, before owner-local name resolution and CEC admission.
// These are IPC/transport requests, never an outgoing CEC frame backlog.
enum class OwnedIntentKind {
    power, source_input, switch_input, announce_active_source, query_audio,
    key_down, key_up, context, standby_wire, discover
};
struct OwnedQuirk {
    uint32_t vendor_id = 0;
    std::vector<uint8_t> power_on, power_off;
};
struct OwnedIntent {
    OwnedIntentKind kind = OwnedIntentKind::query_audio;
    // Port-only actions (context, announcement, standby and discovery) may
    // leave osd_name empty. All component/key actions require an actual name.
    QualifiedDeviceName target;
    uint64_t generation = 0, sequence = 0;
    uint32_t setup_generation = 0;
    HoldToken token;
    uint8_t key = 0, input = 0;
    bool repeat = false, on = false, apply_quirks = true;
    std::vector<OwnedQuirk> quirks;
    // Non-emitting context mirrors the original controller's selected activity
    // for follower replies and play/pause state, without replaying its recipe.
    bool selected = false, source_changed = false;
    std::optional<QualifiedDeviceName> source, audio, display, switch_device;
};
enum class OwnedAuthority {control,realtime,cleanup};
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
    std::optional<std::vector<OwnedQuirk>> quirks = std::nullopt;
};
struct OwnedDispatchResult {
    bool admitted = false;
    std::optional<bool> source_present;
    uint64_t sequence = 0;
    OwnedDispatchResult(bool accepted=false,std::optional<bool> present={},uint64_t serial=0)
        : admitted(accepted),source_present(present),sequence(serial) {}
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
    uint32_t setup_generation = 0, setup_pending = 0;
    bool setup_replies_pending = false;
    uint64_t setup_last_sequence = 0;
    bool audio_status_known = false, muted = false;
    uint8_t volume = 127;
    std::vector<OwnedDeviceObservation> devices;
};
} // namespace amity::system
