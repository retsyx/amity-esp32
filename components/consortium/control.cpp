#include "amity/consortium/control.hpp"
#include <set>

namespace amity::consortium {
namespace {
bool digest(std::string_view value) { return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }); }
bool text(const Json* v, std::initializer_list<std::string_view> options) { return v && v->type == Json::Type::string && std::find(options.begin(), options.end(), v->string) != options.end(); }
std::string base(const PlanContext& c) {
    if (!c.plan_id || !c.session_id || !c.route_generation || !c.policy_generation || !digest(c.plan_digest) || !digest(c.policy_hash)) return {};
    return "\"plan_id\":\"" + c.plan_id.str() + "\",\"session_id\":\"" + c.session_id.str() + "\",\"route_generation\":\"" + model::format_uint64(c.route_generation) +
        "\",\"policy_generation\":\"" + model::format_uint64(c.policy_generation) + "\",\"plan_digest\":\"" + c.plan_digest + "\",\"policy_hash\":\"" + c.policy_hash + "\"";
}
bool context(const Json& v, PlanContext& c) {
    return id(v.get("plan_id"), c.plan_id) && id(v.get("session_id"), c.session_id) && decimal(v.get("route_generation"), c.route_generation) && c.route_generation &&
        decimal(v.get("policy_generation"), c.policy_generation) && c.policy_generation && json_string(v.get("plan_digest"), c.plan_digest, 64, 64) && digest(c.plan_digest) &&
        json_string(v.get("policy_hash"), c.policy_hash, 64, 64) && digest(c.policy_hash);
}
}
Error parse_plan_context(const Json& v, PlanContext& c) {
    return closed(v, {"plan_id", "session_id", "route_generation", "policy_generation", "plan_digest", "policy_hash"}) && context(v, c) ? Error::none : Error::invalid_message;
}
Error parse_plan_prepare(const Json& v, PlanPrepare& out) {
    if (!closed(v, {"plan_id", "session_id", "route_generation", "policy_generation", "plan_digest", "policy_hash", "activity_id", "zone_id", "route_id", "operation_capacity", "bindings", "grants"})) return Error::invalid_message;
    PlanPrepare p; uint64_t capacity;
    if (!context(v, p.context) || !id(v.get("activity_id"), p.activity_id) || !id(v.get("zone_id"), p.zone_id) || !id(v.get("route_id"), p.route_id) || !json_uint(v.get("operation_capacity"), capacity, 1, 64)) return Error::invalid_message;
    p.operation_capacity = static_cast<uint16_t>(capacity);
    const auto* bindings = v.get("bindings"), *grants = v.get("grants");
    if (bindings->type != Json::Type::array || bindings->array.empty() || bindings->array.size() > 8 || grants->type != Json::Type::array || grants->array.size() > 8) return Error::invalid_message;
    std::set<model::BindingId> seen, granted;
    for (const auto& b : bindings->array) {
        BindingRequirement r;
        if (!closed(b, {"binding_id", "component_id", "domain_id", "binding_revision", "domain_revision", "target"}) || !id(b.get("binding_id"), r.binding_id) ||
            !id(b.get("component_id"), r.component_id) || !id(b.get("domain_id"), r.domain_id) || !decimal(b.get("binding_revision"), r.binding_revision) || !r.binding_revision ||
            !decimal(b.get("domain_revision"), r.domain_revision) || !r.domain_revision || !parse_address(*b.get("target"), r.target) || !seen.insert(r.binding_id).second) return Error::invalid_message;
        p.bindings.push_back(r);
    }
    for (const auto& g : grants->array) {
        PreparedGrant grant;
        if (!closed(g, {"binding_id", "permitted_paths"}) || !id(g.get("binding_id"), grant.binding_id) || !seen.count(grant.binding_id) || !granted.insert(grant.binding_id).second) return Error::invalid_message;
        const auto* paths = g.get("permitted_paths"); if (paths->type != Json::Type::array || paths->array.size() > 16) return Error::invalid_message;
        std::set<uint16_t> unique;
        for (const auto& path : paths->array) { uint64_t value; if (!json_uint(&path, value, 1, 65534) || !model::valid_physical_address(static_cast<uint16_t>(value), false) || !unique.insert(static_cast<uint16_t>(value)).second) return Error::invalid_message; grant.permitted_paths.push_back(static_cast<uint16_t>(value)); }
        p.grants.push_back(std::move(grant));
    }
    out = std::move(p); return Error::none;
}
Error parse_plan_barrier(const Json& v, PlanBarrier& out) {
    PlanBarrier b;
    if (!context(v, b.context) || !json_string(v.get("phase"), b.phase, 1, 16)) return Error::invalid_message;
    if (b.phase == "fence") {
        model::SessionId old;
        if (!closed(v, {"plan_id", "session_id", "route_generation", "policy_generation", "plan_digest", "policy_hash", "phase"}, {"old_session_id", "old_route_generation"})) return Error::invalid_message;
        if (!v.get("old_session_id") && !v.get("old_route_generation")) { out=std::move(b); return Error::none; }
        if (!id(v.get("old_session_id"), old) || !decimal(v.get("old_route_generation"), b.old_route_generation) || !b.old_route_generation) return Error::invalid_message;
        b.old_session_id = old;
    } else if (b.phase == "activate" || b.phase == "renew") {
        uint64_t ttl;
        if (b.phase == "activate") { if (!closed(v, {"plan_id", "session_id", "route_generation", "policy_generation", "plan_digest", "policy_hash", "phase", "authority_ttl_ms"})) return Error::invalid_message; }
        else if (!closed(v, {"plan_id", "session_id", "route_generation", "policy_generation", "plan_digest", "policy_hash", "phase", "authority_ttl_ms", "execution_lease_id"}) || !id(v.get("execution_lease_id"), b.execution_lease_id)) return Error::invalid_message;
        if (!json_uint(v.get("authority_ttl_ms"), ttl, 1, 5000)) return Error::invalid_message;
        b.authority_ttl_ms = static_cast<uint32_t>(ttl);
    } else return Error::invalid_message;
    out = std::move(b); return Error::none;
}
Error parse_plan_result(const Json& v, PlanResult& out) {
    PlanResult result;
    if (!closed(v, {"plan_id", "session_id", "route_generation", "policy_generation", "plan_digest", "policy_hash", "phase", "result"}, {"error"}) || !context(v, result.context) ||
        !json_string(v.get("phase"), result.phase, 1, 16) || !text(v.get("phase"), {"prepare", "fence", "activate", "abort"}) ||
        !json_string(v.get("result"), result.result, 1, 16) || !text(v.get("result"), {"ready", "quiescent", "applied", "aborted", "pending", "rejected"})) return Error::invalid_message;
    if (result.result == "rejected") {
        if (!json_string(v.get("error"), result.error, 1, 32)) return Error::invalid_message;
        bool known = false; for (auto e : {Error::invalid_message, Error::stale_boot, Error::stale_epoch, Error::stale_config, Error::stale_binding, Error::stale_domain, Error::stale_route, Error::queue_full, Error::not_ready, Error::unauthorized_role}) if (result.error == error_name(e)) known = true;
        if (!known) return Error::invalid_message;
    } else if (v.get("error")) return Error::invalid_message;
    if ((result.result == "ready" && result.phase != "prepare") || (result.result == "quiescent" && result.phase != "fence") ||
        (result.result == "applied" && result.phase != "activate") || (result.result == "aborted" && result.phase != "abort")) return Error::invalid_message;
    out = std::move(result); return Error::none;
}
bool valid_config_control(Kind kind, const Json& v) {
    if (!closed(v, {"config_revision", "presentation_revision", "owner_boot_id", "payload_hash", "policy_hash", "phase"}, {"result", "error"})) return false;
    uint64_t config, presentation; model::BootId boot; std::string payload, policy;
    if (!decimal(v.get("config_revision"), config) || !config || !decimal(v.get("presentation_revision"), presentation) || !presentation || !id(v.get("owner_boot_id"), boot) ||
        !json_string(v.get("payload_hash"), payload, 64, 64) || !digest(payload) || !json_string(v.get("policy_hash"), policy, 64, 64) || !digest(policy)) return false;
    if (kind == Kind::config_prepare) return text(v.get("phase"), {"prepare"}) && !v.get("result") && !v.get("error");
    if (kind == Kind::config_activate) return text(v.get("phase"), {"activate"}) && !v.get("result") && !v.get("error");
    if (kind != Kind::config_result || !text(v.get("phase"), {"prepare", "activate"}) || !text(v.get("result"), {"ready", "applied", "rejected"})) return false;
    return text(v.get("result"), {"rejected"}) ? text(v.get("error"), {"STALE_BOOT", "STALE_CONFIG", "INVALID_MESSAGE", "NOT_READY", "STORAGE_FAILURE", "QUEUE_FULL"}) : !v.get("error");
}
bool valid_operation_result(const Json& v) {
    model::CommandId command; model::PlanId plan; model::SessionId session; uint64_t generation;
    if (!closed(v, {"command_id", "plan_id", "session_id", "route_generation", "result"}, {"error"}) || !id(v.get("command_id"), command) || !id(v.get("plan_id"), plan) || !id(v.get("session_id"), session) || !decimal(v.get("route_generation"), generation) || !generation ||
        !text(v.get("result"), {"admitted", "applied", "observed", "rejected", "timed_out", "unknown"})) return false;
    if (text(v.get("result"), {"rejected", "timed_out", "unknown"})) return !v.get("error") || text(v.get("error"), {"STALE_BOOT", "STALE_CONFIG", "STALE_BINDING", "STALE_DOMAIN", "STALE_ROUTE", "LEASE_EXPIRED", "NOT_READY", "QUEUE_FULL", "INVALID_MESSAGE", "UNSUPPORTED_CAPABILITY", "UNKNOWN_OUTCOME", "nack", "arbitration_lost", "canceled", "expired", "line_error", "admission_rejected"});
    return !v.get("error");
}
std::string plan_prepare_json(const PlanPrepare& p) {
    const auto common = base(p.context); if (common.empty() || p.bindings.empty() || p.bindings.size() > 8 || p.grants.size() > 8) return {};
    std::string out = "{" + common + ",\"activity_id\":\"" + p.activity_id.str() + "\",\"zone_id\":\"" + p.zone_id.str() + "\",\"route_id\":\"" + p.route_id.str() + "\",\"operation_capacity\":" + std::to_string(p.operation_capacity) + ",\"bindings\":[";
    bool first = true; for (const auto& b : p.bindings) {
        if (!first) out += ',';
        first = false;
        out += "{\"binding_id\":\"" + b.binding_id.str() + "\",\"component_id\":\"" + b.component_id.str() + "\",\"domain_id\":\"" + b.domain_id.str() + "\",\"binding_revision\":\"" + model::format_uint64(b.binding_revision) + "\",\"domain_revision\":\"" + model::format_uint64(b.domain_revision) + "\",\"target\":" + model::address_json(b.target) + "}";
    }
    out += "],\"grants\":["; first = true;
    for (const auto& grant : p.grants) { if (!first) out += ','; first = false; out += "{\"binding_id\":\"" + grant.binding_id.str() + "\",\"permitted_paths\":["; bool first_path = true; for (auto path : grant.permitted_paths) { if (!first_path) out += ','; first_path = false; out += std::to_string(path); } out += "]}"; }
    out += "]}"; return out;
}
std::string plan_barrier_json(const PlanBarrier& b) {
    auto common = base(b.context); if (common.empty()) return {};
    if (b.phase == "fence" && !b.old_session_id && !b.old_route_generation) return "{" + common + ",\"phase\":\"fence\"}";
    if (b.phase == "fence" && b.old_session_id && b.old_route_generation) return "{" + common + ",\"phase\":\"fence\",\"old_session_id\":\"" + b.old_session_id->str() + "\",\"old_route_generation\":\"" + model::format_uint64(b.old_route_generation) + "\"}";
    if (b.phase == "activate" && b.authority_ttl_ms && b.authority_ttl_ms <= 5000) return "{" + common + ",\"phase\":\"activate\",\"authority_ttl_ms\":" + std::to_string(b.authority_ttl_ms) + "}";
    if (b.phase == "renew" && b.execution_lease_id && b.authority_ttl_ms && b.authority_ttl_ms <= 5000) return "{" + common + ",\"phase\":\"renew\",\"authority_ttl_ms\":" + std::to_string(b.authority_ttl_ms) + ",\"execution_lease_id\":\"" + b.execution_lease_id.str() + "\"}";
    return {};
}
std::string plan_abort_json(const PlanContext& context) { const auto value = base(context); return value.empty() ? std::string{} : "{" + value + "}"; }
std::string plan_result_json(const PlanResult& result) {
    auto value = base(result.context); if (value.empty()) return {};
    value = "{" + value + ",\"phase\":\"" + result.phase + "\",\"result\":\"" + result.result + "\"";
    if (!result.error.empty()) value += ",\"error\":\"" + result.error + "\"";
    return value + "}";
}
std::string config_control_json(uint64_t config, uint64_t presentation, model::BootId boot, std::string_view payload, std::string_view policy, std::string_view phase) {
    if (!config || !presentation || !boot || !digest(payload) || !digest(policy) || (phase != "prepare" && phase != "activate")) return {};
    return "{\"config_revision\":\"" + model::format_uint64(config) + "\",\"presentation_revision\":\"" + model::format_uint64(presentation) + "\",\"owner_boot_id\":\"" + boot.str() + "\",\"payload_hash\":\"" + std::string(payload) + "\",\"policy_hash\":\"" + std::string(policy) + "\",\"phase\":\"" + std::string(phase) + "\"}";
}
} // namespace amity::consortium
