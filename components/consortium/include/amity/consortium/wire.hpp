// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/consortium/json.hpp"
#include "amity/system/model.hpp"
#include "amity/system/owned_intent.hpp"
#include <optional>

namespace amity::consortium {
namespace model = amity::system;
enum class Error { none, invalid_message, unsupported_kind, unauthorized_role,
    wrong_consortium, stale_boot, stale_config, expired, target_not_local, not_ready, queue_full,
    identity_conflict, storage_failure, transaction_conflict };
const char *error_name(Error);
enum class Kind { enrollment_control,
    membership_event, membership_result,
    update_begin, update_chunk, update_install, update_abort, update_result,
    device_status, settings_read, settings_write, settings_result,
    scoped_intent, scoped_result, synchronization, log_request, log_result };
const char *kind_name(Kind);
struct UpdateChunk {
    model::MessageId begin_id;
    uint64_t offset = 0;
    std::string bytes;
};
struct LogChunk {
    model::MessageId transfer;
    uint32_t offset = 0, total = 0;
    bool unavailable = false;
    std::string bytes;
};
struct AdapterSettingChange {uint8_t tentacle_id=0;std::string default_front_physical_address;};
struct DeviceSettingsWrite {model::BootId expected_boot;uint64_t expected_revision=0,expected_hostname_revision=0;std::vector<AdapterSettingChange> changes;std::optional<std::string> hostname;};
// The authenticated connection identifies both Amities and boots; a frame
// carries only its configuration fence, kind, lifetime and payload.
struct Envelope {
    uint64_t config_revision = 0;
    Kind kind = Kind::device_status;
    uint32_t ttl_ms = 0;
    Json payload;
    // scoped_intent and settings_write payloads, decoded once when the frame is parsed.
    std::optional<model::OwnedIntent> intent;
    std::optional<DeviceSettingsWrite> settings_write;
    std::optional<UpdateChunk> update_chunk;
    std::optional<LogChunk> log_chunk;
};
// Firmware and log bytes travel as bulk frames, with longer transfer budgets.
bool is_bulk_kind(Kind);
// Supported kinds all have closed payload validators.
Error parse_envelope(std::string_view body, Envelope &out, JsonError *detail = nullptr);
bool valid_envelope_payload(Kind, const Json&);
// A member's station signal in device_status. Protocol integers are unsigned,
// so the dBm value travels negated.
struct WifiSignalReport { int8_t rssi = 0; uint8_t channel = 0; };
std::string wifi_signal_json(const WifiSignalReport &);
std::optional<WifiSignalReport> parse_wifi_signal(const Json &device_status);
Error parse_synchronization(const Json &,model::MessageId& round,std::string& phase,uint64_t& revision);
std::string synchronization_json(model::MessageId round,std::string_view phase,uint64_t revision);
bool decimal(const Json *, uint64_t &);
template<class Id> bool id(const Json *v, Id &out) {
    if(!v || v->type!=Json::Type::string) return false;
    auto parsed=Id::parse(v->string); if(!parsed)return false;out=*parsed;return true;
}
struct HelloFrame {
    model::ConsortiumId consortium_id;
    model::OctopusId octopus_id;
    model::BootId boot_id;
    uint64_t config_revision = 0;
    std::string firmware_identity;
    std::optional<model::JoinId> pending_join_id;std::string join_digest;
    // Assigned GPIO/profile availability, never proof of a plugged-in adapter.
    uint8_t supported_tentacles = 1, configured_tentacles_mask = 1;
};
std::string hello_json(const HelloFrame &);
Error parse_hello(std::string_view, HelloFrame &, JsonError *detail = nullptr);
} // namespace amity::consortium
