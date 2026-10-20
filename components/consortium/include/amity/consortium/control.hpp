#pragma once
#include "amity/consortium/wire.hpp"
#include "amity/system/policy.hpp"

namespace amity::consortium {
struct PlanContext {
    model::PlanId plan_id;
    model::SessionId session_id;
    uint64_t route_generation = 0, policy_generation = 0;
    std::string plan_digest, policy_hash;
};
struct BindingRequirement {
    model::BindingId binding_id;
    model::ComponentId component_id;
    model::DomainId domain_id;
    uint64_t binding_revision = 0, domain_revision = 0;
    model::FullyQualifiedCecAddress target;
};
struct PreparedGrant { model::BindingId binding_id; std::vector<uint16_t> permitted_paths; };
struct PlanPrepare {
    PlanContext context;
    model::ActivityId activity_id;
    model::ZoneId zone_id;
    model::RouteId route_id;
    uint16_t operation_capacity = 0;
    std::vector<BindingRequirement> bindings;
    std::vector<PreparedGrant> grants;
};
struct PlanBarrier {
    PlanContext context;
    std::string phase;
    std::optional<model::SessionId> old_session_id;
    uint64_t old_route_generation = 0;
    uint32_t authority_ttl_ms = 0;
    model::ExecutionLeaseId execution_lease_id;
};
struct PlanResult { PlanContext context; std::string phase, result, error; };
Error parse_plan_prepare(const Json&, PlanPrepare&);
Error parse_plan_barrier(const Json&, PlanBarrier&);
Error parse_plan_context(const Json&, PlanContext&);
Error parse_plan_result(const Json&, PlanResult&);
bool valid_config_control(Kind, const Json&);
bool valid_operation_result(const Json&);
std::string plan_prepare_json(const PlanPrepare&);
std::string plan_barrier_json(const PlanBarrier&);
std::string plan_abort_json(const PlanContext&);
std::string plan_result_json(const PlanResult&);
std::string config_control_json(uint64_t config_revision, uint64_t presentation_revision,
    model::BootId owner_boot, std::string_view payload_hash, std::string_view policy_hash, std::string_view phase);
} // namespace amity::consortium
