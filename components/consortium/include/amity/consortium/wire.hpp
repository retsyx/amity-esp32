// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/consortium/json.hpp"
#include "amity/system/model.hpp"
#include <optional>

namespace amity::consortium {
namespace model = amity::system;
enum class Error { none, invalid_message, unsupported_kind, protocol_mismatch, unauthorized_role,
    wrong_consortium, stale_boot, stale_epoch, stale_config, stale_binding, stale_domain,
    stale_route, lease_expired, target_not_local, not_ready, queue_full, duplicate,
    identity_conflict, unknown_outcome, storage_failure, transaction_conflict };
const char *error_name(Error);
// Numeric slots of retired features are never reassigned or dispatched.
enum class Kind { heartbeat=0, execution_lease=1, operation=3, hold_release=5,
    relay_reserve=7, relay_record=8, relay_close=9, relay_result=10, enrollment_control=11,
    binding_update=12, binding_result=13, membership_event=14, membership_result=15,
    snapshot_request=16, snapshot_chunk=17, config_prepare=18, config_activate=19, config_result=20,
    plan_prepare=21, plan_barrier=22, plan_abort=23, plan_result=24, operation_result=25, update_begin=27, update_chunk=28, update_install=29, update_status=30, update_abort=31, update_result=32,
    scan_request=33, scan_result=34, device_status=37, settings_read=38, settings_write=39, settings_result=40 };
// Retired slots2/4/6/26/35/36 are never reassigned. Cast integer inputs cannot
// reactivate them; old JSON kind strings are unrecognized.
inline bool retired_kind(Kind k){const auto n=static_cast<unsigned>(k);return n==2||n==4||n==6||n==26||n==35||n==36;}
const char *kind_name(Kind);
enum class LeaseClass { control, realtime_input };
struct Envelope {
    model::ConsortiumId consortium_id;
    model::MessageId message_id;
    model::OctopusId origin_octopus_id, destination_octopus_id;
    model::BootId origin_boot_id, destination_boot_id;
    uint64_t coordinator_epoch = 0, config_revision = 0;
    Kind kind = Kind::heartbeat;
    uint32_t ttl_ms = 0;
    Json payload;
};
enum class SemanticOperation { ensure_power, select_route, announce_active_source, key_press, query_state };
struct Operation {
    model::CommandId command_id;
    uint64_t command_sequence=0;
    model::PlanId plan_id;
    model::SessionId session_id;
    model::ComponentId component_id;
    model::BindingId binding_id;
    model::DomainId domain_id;
    model::ExecutionLeaseId execution_lease_id;
    uint64_t route_generation = 0, binding_revision = 0, domain_revision = 0;
    model::FullyQualifiedCecAddress target;
    SemanticOperation operation = SemanticOperation::query_state;
    bool on = false, repeat = false;
    model::RouteId route_id;
    model::HoldToken hold_token;
    uint8_t key_code = 0;
    std::string query_field;
};
// Supported kinds all have closed payload validators. Other registered v1 kinds
// remain unavailable until a reviewed payload contract is implemented.
Error parse_envelope(std::string_view body, Envelope &out, JsonError *detail = nullptr);
Error parse_operation(const Json &payload, Operation &out);
bool parse_address(const Json &, model::FullyQualifiedCecAddress &);
bool parse_name(const Json &, model::QualifiedDeviceName &);
bool decimal(const Json *, uint64_t &);
template<class Id> bool id(const Json *v, Id &out) {
    if(!v || v->type!=Json::Type::string) return false;
    auto parsed=Id::parse(v->string); if(!parsed)return false;out=*parsed;return true;
}
// Unknown required capabilities fail; unknown optional bits are ignored.
enum Capability : uint32_t { semantic_operations = 1u, token_holds = 2u,
    binding_synchronization = 4u, lifecycle_journal = 8u, source_isolation_v1 = 16u,
    prepared_plans = 32u, chunked_snapshots = 64u, configuration_activation = 128u, operation_outcomes = 256u, peer_firmware_update = 512u, sequenced_operations = 1024u, bootstrap_relay = 2048u, original_controller_only = 8192u, hostname_metadata = 16384u, per_adapter_settings = 32768u, factory_reset_removal = 65536u, owner_repeated_keys = 131072u };
constexpr uint32_t known_capabilities = 258047u;
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
