// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/consortium/json.hpp"
#include "amity/system/model.hpp"
#include <optional>

namespace amity::consortium {
namespace model = amity::system;
enum class Error { none, invalid_message, unsupported_kind, protocol_mismatch, unauthorized_role,
    wrong_consortium, stale_boot, stale_epoch, stale_config, expired, target_not_local, not_ready, queue_full, duplicate,
    identity_conflict, unknown_outcome, storage_failure, transaction_conflict };
const char *error_name(Error);
// Numeric slots of retired features are never reassigned or dispatched.
enum class Kind { enrollment_control=11,
    membership_event=14, membership_result=15, snapshot_request=16, snapshot_chunk=17,
    update_begin=27, update_chunk=28, update_install=29, update_status=30, update_abort=31, update_result=32,
    device_status=37, settings_read=38, settings_write=39, settings_result=40,
    scoped_intent=41, scoped_result=42, synchronization=43, log_request=44, log_result=45 };
inline bool retired_kind(Kind k){const auto n=static_cast<unsigned>(k);return n<=10||n==12||n==13||(n>=18&&n<=26)||(n>=33&&n<=36);}
const char *kind_name(Kind);
struct UpdateChunk {
    model::MessageId begin_id;
    uint64_t offset = 0;
    std::string bytes;
};
struct Envelope {
    model::ConsortiumId consortium_id;
    model::OctopusId origin_octopus_id, destination_octopus_id;
    model::BootId origin_boot_id, destination_boot_id;
    uint64_t coordinator_epoch = 0, config_revision = 0;
    Kind kind = Kind::device_status;
    uint32_t ttl_ms = 0;
    Json payload;
    std::optional<UpdateChunk> update_chunk;
};
// Supported kinds all have closed payload validators. Retired numeric slots reject.
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
// Unknown required capabilities fail; unknown optional bits are ignored.
enum Capability : uint32_t { lifecycle_journal=8u,
    chunked_snapshots=64u, peer_firmware_update=512u,
    original_controller_only=8192u, hostname_metadata=16384u, per_adapter_settings=32768u,
    factory_reset_removal=65536u, owner_repeated_keys=131072u, qualified_lookup=262144u,
    live_configuration=524288u, peer_logs=1048576u };
constexpr uint32_t known_capabilities = lifecycle_journal|chunked_snapshots|peer_firmware_update|original_controller_only|hostname_metadata|per_adapter_settings|factory_reset_removal|owner_repeated_keys|qualified_lookup|live_configuration|peer_logs;
struct HelloFrame {
    uint8_t protocol_min = 1, protocol_max = 1;
    model::ConsortiumId consortium_id;
    model::OctopusId octopus_id;
    model::BootId boot_id;
    uint64_t coordinator_epoch = 0, config_revision = 0;
    std::string config_hash, firmware_identity;
    std::optional<model::JoinId> pending_join_id;std::string join_digest;
    uint32_t required_capabilities = 0, optional_capabilities = 0;
    uint32_t frame_cap = max_frame_bytes;
    // Assigned GPIO/profile availability, never proof of a plugged-in adapter.
    uint8_t supported_tentacles = 1, configured_tentacles_mask = 1;
    bool hardware_profile = false;
};
inline bool supports_capabilities(const HelloFrame &peer,uint32_t required){return (required&~(peer.required_capabilities|peer.optional_capabilities))==0;}
std::string hello_json(const HelloFrame &);
Error parse_hello(std::string_view, HelloFrame &, JsonError *detail = nullptr);
} // namespace amity::consortium
