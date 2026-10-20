#include "amity/system/planner.hpp"
#include <limits>
#include <set>

namespace amity::system {
namespace {
template<class T> bool contains(const std::vector<T>& v, const T& x) { return std::find(v.begin(), v.end(), x) != v.end(); }
bool same_intent(const SelectRequest& a, const SelectRequest& b) {
    return a.zone_id == b.zone_id && a.activity_id == b.activity_id && a.session_id == b.session_id &&
        a.plan_id == b.plan_id && a.expected_state_revision == b.expected_state_revision && a.admissible_until_ms == b.admissible_until_ms;
}
}
ResourcePlanner::ResourcePlanner(const Topology& topology, size_t capacity) : capacity_(std::min<size_t>(capacity, 64)) {
    if (!topology.config_revision || topology.components.size() > 64 || topology.bindings.size() > 128 ||
        topology.resources.size() > 128 || topology.signal_edges.size() > 256 || topology.routes.size() > 64 ||
        topology.activities.size() > 64 || topology.ready_owners.size() > 64) return;
    std::set<ComponentId> components; std::set<BindingId> bindings; std::set<ResourceId> resources;
    std::set<RouteId> routes; std::set<ActivityId> activities;
    for (const auto& c : topology.components) {
        if (!c.component_id || !components.insert(c.component_id).second || c.label.size() > 128 || c.qualified_names.size() > 16) return;
        for (const auto& name : c.qualified_names) if (!name.valid()) return;
    }
    for (const auto& b : topology.bindings) if (!b.binding_id || !bindings.insert(b.binding_id).second) return;
    for (const auto& r : topology.resources) {
        if (!r.resource_id || !resources.insert(r.resource_id).second || !r.owner_octopus_id || r.supported_modes.size() > 32) return;
        for (const auto& mode : r.supported_modes) if (mode.empty() || mode.size() > 64) return;
    }
    for (const auto& r : topology.routes) {
        if (!r.route_id || !routes.insert(r.route_id).second || r.physical_path.size() > 32 || r.demands.size() > 32 ||
            r.required_bindings.size() > 32 || r.steps.size() > 64) return;
        for (const auto& d : r.demands) if (d.mode.size() > 64) return;
        for (const auto& step : r.steps) if (step.dependencies.size() > 32) return;
    }
    for (const auto& a : topology.activities) if (!a.activity_id || !a.zone_id || !a.route_id || !activities.insert(a.activity_id).second) return;
    topology_ = topology; valid_topology_ = true;
}
std::optional<std::string> ResourcePlanner::validate_route(const RouteDefinition& route) const {
    if (!route.route_id || !route.source || !route.display || route.physical_path.size() < 2 ||
        route.physical_path.front().component_id != route.source || route.physical_path.back().component_id != route.display)
        return "route has no complete physical source-to-display path";
    if (route.required_bindings.empty() || route.steps.empty()) return "route requires an explicit supported control plan";
    for (const auto& p : route.physical_path) {
        if (!p.component_id || (p.direction != SignalDirection::input && p.direction != SignalDirection::output) ||
            std::none_of(topology_.components.begin(), topology_.components.end(), [&](const Component& c) { return c.component_id == p.component_id; }))
            return "physical path references unconfigured equipment";
    }
    for (size_t i = 1; i < route.physical_path.size(); ++i) {
        bool supported = false;
        for (const auto& edge : topology_.signal_edges) if (edge.from == route.physical_path[i - 1] && edge.to == route.physical_path[i] && edge.support == Support::supported) {
            const bool cable = edge.from.direction == SignalDirection::output && edge.to.direction == SignalDirection::input;
            const bool internal = edge.from.component_id == edge.to.component_id && edge.from.direction == SignalDirection::input && edge.to.direction == SignalDirection::output;
            if ((edge.kind == SignalEdgeKind::hdmi_cable && cable) || (edge.kind == SignalEdgeKind::internal_equipment_path && internal)) supported = true;
        }
        if (!supported) return "missing or unverified physical/internal HDMI path";
    }
    if (route.requires_return_audio && route.return_audio_support != Support::supported) return "return audio capability is unqualified";
    std::set<ResourceId> demanded;
    for (const auto& d : route.demands) {
        if (!d.resource_id || !demanded.insert(d.resource_id).second) return "duplicate or invalid resource demand";
        auto resource = std::find_if(topology_.resources.begin(), topology_.resources.end(), [&](const Resource& r) { return r.resource_id == d.resource_id; });
        if (resource == topology_.resources.end() || !resource->owner_octopus_id || !contains(resource->supported_modes, d.mode) ||
            !contains(topology_.ready_owners, resource->owner_octopus_id) ||
            (resource->compatibility == ResourceCompatibility::control_owner && !d.control_owner)) return "resource mode or owner unavailable";
    }
    for (const auto& id : route.required_bindings) {
        auto binding = std::find_if(topology_.bindings.begin(), topology_.bindings.end(), [&](const ComponentBinding& b) { return b.binding_id == id; });
        if (binding == topology_.bindings.end() || binding->status != BindingStatus::verified || !contains(topology_.ready_owners, binding->owner_octopus_id)) return "required binding unresolved or owner unavailable";
        auto component = std::find_if(topology_.components.begin(), topology_.components.end(), [&](const Component& c) { return c.component_id == binding->component_id; });
        if (component == topology_.components.end() || !valid_binding(*binding, *component)) return "required binding violates configured qualified name";
    }
    for (size_t i = 0; i < route.steps.size(); ++i) {
        if (!contains(route.required_bindings, route.steps[i].binding_id)) return "plan step binding is not a validated route requirement";
        const auto binding = std::find_if(topology_.bindings.begin(), topology_.bindings.end(), [&](const ComponentBinding& b) { return b.binding_id == route.steps[i].binding_id; });
        const auto kind = route.steps[i].operation;
        if ((kind == OperationKind::announce_active_source && (binding->kind != BindingKind::proxy_representation || binding->capability != Capability::proxy_activation)) ||
            (kind == OperationKind::select_path && (binding->role != PortRole::tv_controller || binding->capability != Capability::routing)) ||
            (kind == OperationKind::ensure_power && binding->capability != Capability::power) ||
            ((kind == OperationKind::press_key || kind == OperationKind::release_key) && binding->capability != Capability::navigation && binding->capability != Capability::audio && binding->capability != Capability::volume))
            return "plan operation is incompatible with its binding capability or role";
        for (auto dependency : route.steps[i].dependencies) if (dependency >= i) return "dependency graph is cyclic or out of order";
    }
    return {};
}
bool ResourcePlanner::compatible(const ResourceDemand& a, const ResourceDemand& b) const {
    if (a.resource_id != b.resource_id) return true;
    for (const auto& resource : topology_.resources) if (resource.resource_id == a.resource_id) {
        if (resource.compatibility == ResourceCompatibility::exclusive) return false;
        if (resource.compatibility == ResourceCompatibility::control_owner) return a.mode == b.mode && a.control_owner == b.control_owner;
        return a.mode == b.mode;
    }
    return false;
}
PlanningResult ResourcePlanner::select_activity(const SelectRequest& request) {
    if (!valid_topology_) return {PlanningStatus::unresolved, {}, {}, "topology is invalid or exceeds bounded capacity"};
    if (state_revision_ == std::numeric_limits<uint64_t>::max()) return {PlanningStatus::stale, {}, {}, "state revision exhausted"};
    for (const auto& cached : requests_) if (cached.request.request_id == request.request_id) {
        if (!same_intent(cached.request, request)) return {PlanningStatus::stale, {}, {}, "request ID reused with different intent"};
        auto result = cached.result; result.status = PlanningStatus::duplicate; return result;
    }
    if (!request.request_id || !request.zone_id || !request.activity_id || !request.session_id || !request.plan_id)
        return {PlanningStatus::unresolved, {}, {}, "fresh intent/session/plan IDs are required"};
    if (request.expected_state_revision && *request.expected_state_revision != state_revision_)
        return {PlanningStatus::stale, {}, {}, "state changed since the request was prepared"};
    if (requests_.size() >= capacity_) {
        // Exact duplicates of active/uncertain work stay pinned. A retired
        // request with an expected old state cannot reserve a new plan even
        // after its cached response is evicted: the state check above rejects it.
        auto retired = std::find_if(requests_.begin(), requests_.end(), [&](const auto& cached) {
            if (!cached.request.expected_state_revision || *cached.request.expected_state_revision >= state_revision_) return false;
            const auto id = cached.request.plan_id;
            return std::none_of(prepared_.begin(), prepared_.end(), [&](const auto& p) { return p.plan_id == id; }) &&
                std::none_of(sessions_.begin(), sessions_.end(), [&](const auto& s) { return s.plan_id == id; });
        });
        if (retired != requests_.end()) requests_.erase(retired);
    }
    if (requests_.size() >= capacity_ || prepared_.size() + sessions_.size() >= capacity_)
        return {PlanningStatus::overloaded, {}, {}, "planner capacity exhausted"};
    auto activity = std::find_if(topology_.activities.begin(), topology_.activities.end(), [&](const ActivityDefinition& a) {
        return a.activity_id == request.activity_id && a.zone_id == request.zone_id;
    });
    if (activity == topology_.activities.end()) return {PlanningStatus::unresolved, {}, {}, "activity does not belong to the requested zone"};
    auto route = std::find_if(topology_.routes.begin(), topology_.routes.end(), [&](const RouteDefinition& r) { return r.route_id == activity->route_id; });
    if (route == topology_.routes.end()) return {PlanningStatus::unresolved, {}, {}, "activity route missing"};
    if (auto issue = validate_route(*route)) return {PlanningStatus::unsupported, {}, {}, *issue};
    Plan plan;
    plan.plan_id = request.plan_id; plan.config_revision = topology_.config_revision;
    plan.execution_expires_at_ms = request.admissible_until_ms;
    plan.steps = route->steps;
    plan.requires_topology_revalidation = route->changes_physical_addresses && !route->qualified_physical_address_stability;
    // Do not release a PA-dependent plan on the old address after output changes.
    if (plan.requires_topology_revalidation) return {PlanningStatus::unresolved, {}, {}, "output change requires staged physical-address revalidation before routing"};
    uint64_t next_generation = 1;
    for (const auto& session : sessions_) {
        if (session.session_id == request.session_id || session.plan_id == request.plan_id) return {PlanningStatus::stale, {}, {}, "runtime session and plan IDs must not be reused"};
        if (session.zone_id == request.zone_id) {
            if (session.route_generation == std::numeric_limits<uint64_t>::max()) return {PlanningStatus::stale, {}, {}, "route generation exhausted"};
            plan.replaces_session = session.session_id; next_generation = session.route_generation + 1;
        }
    }
    for (const auto& prepared : prepared_) if (prepared.proposed_session.zone_id == request.zone_id || prepared.plan_id == request.plan_id || prepared.proposed_session.session_id == request.session_id)
        return {PlanningStatus::conflict, {}, {prepared.proposed_session.session_id}, "another transition already reserves this zone or runtime ID"};
    plan.proposed_session = {request.session_id, request.activity_id, request.zone_id, route->route_id, request.plan_id,
        next_generation, SessionStatus::preparing, route->demands};
    PlanningResult result{PlanningStatus::proposed, {}, {}, {}};
    auto check = [&](const Session& session) {
        if (plan.replaces_session && session.session_id == *plan.replaces_session) return;
        for (const auto& a : route->demands) for (const auto& b : session.demands) if (!compatible(a, b)) {
            if (!contains(result.conflicting_sessions, session.session_id)) result.conflicting_sessions.push_back(session.session_id);
        }
    };
    for (const auto& session : sessions_) check(session);
    for (const auto& prepared : prepared_) check(prepared.proposed_session);
    if (!result.conflicting_sessions.empty()) {
        result.status = PlanningStatus::conflict; result.reason = "requested resource modes conflict with existing sessions";
        requests_.push_back({request, result}); return result;
    }
    for (const auto& id : route->required_bindings) for (const auto& binding : topology_.bindings)
        if (binding.binding_id == id && !contains(plan.owners, binding.owner_octopus_id)) plan.owners.push_back(binding.owner_octopus_id);
    for (const auto& d : route->demands) for (const auto& resource : topology_.resources)
        if (resource.resource_id == d.resource_id && !contains(plan.owners, resource.owner_octopus_id)) plan.owners.push_back(resource.owner_octopus_id);
    prepared_.push_back(plan); result.plan = plan; requests_.push_back({request, result}); ++state_revision_;
    return result;
}
bool ResourcePlanner::commit_plan(PlanId id, bool observed) {
    if (state_revision_ == std::numeric_limits<uint64_t>::max()) return false;
    auto plan = std::find_if(prepared_.begin(), prepared_.end(), [&](const Plan& p) { return p.plan_id == id; });
    if (plan == prepared_.end()) return false;
    if (plan->replaces_session) sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(), [&](const Session& s) { return s.session_id == *plan->replaces_session; }), sessions_.end());
    auto session = plan->proposed_session; session.status = observed ? SessionStatus::active : SessionStatus::degraded;
    sessions_.push_back(std::move(session)); prepared_.erase(plan); ++state_revision_; return true;
}
bool ResourcePlanner::abort_plan(PlanId id) {
    if (state_revision_ == std::numeric_limits<uint64_t>::max()) return false;
    auto p = std::find_if(prepared_.begin(), prepared_.end(), [&](const Plan& plan) { return plan.plan_id == id; });
    if (p == prepared_.end()) return false;
    prepared_.erase(p); ++state_revision_; return true;
}
std::vector<ResourceId> ResourcePlanner::stop_session(SessionId id) {
    std::vector<ResourceId> released;
    if (state_revision_ == std::numeric_limits<uint64_t>::max()) return released;
    auto session = std::find_if(sessions_.begin(), sessions_.end(), [&](const Session& s) { return s.session_id == id; });
    if (session == sessions_.end()) return released;
    auto previous = session->demands; sessions_.erase(session);
    for (const auto& d : previous) {
        bool still_needed = false;
        for (const auto& s : sessions_) for (const auto& demand : s.demands) if (demand.resource_id == d.resource_id) still_needed = true;
        for (const auto& p : prepared_) for (const auto& demand : p.proposed_session.demands) if (demand.resource_id == d.resource_id) still_needed = true;
        if (!still_needed) released.push_back(d.resource_id);
    }
    ++state_revision_; return released; // A resource release is not a power-off command.
}
void ResourcePlanner::suspend_after_restart() {
    prepared_.clear(); requests_.clear();
    for (auto& s : sessions_) { s.status = SessionStatus::suspended_after_restart; s.demands.clear(); }
    if (state_revision_ != std::numeric_limits<uint64_t>::max()) ++state_revision_;
}
bool ResourcePlanner::update_bindings(const Topology& topology) {
    ResourcePlanner checked(topology, capacity_);
    if (!checked.valid_topology_ || topology.config_revision != topology_.config_revision || !prepared_.empty()) return false;
    topology_ = topology;
    if (state_revision_ != std::numeric_limits<uint64_t>::max()) ++state_revision_;
    return true;
}
bool ResourcePlanner::can_rebase_revision(uint64_t expected, uint64_t next) const {
    return expected && topology_.config_revision == expected && next > expected && prepared_.empty();
}
bool ResourcePlanner::rebase_revision(uint64_t expected, uint64_t next) {
    if (!can_rebase_revision(expected, next)) return false;
    topology_.config_revision = next;
    return true;
}
std::vector<ActivityId> ResourcePlanner::suggested_activities(ZoneId zone) const {
    std::vector<ActivityId> result;
    for (const auto& a : topology_.activities) if (a.zone_id == zone) for (const auto& r : topology_.routes)
        if (r.route_id == a.route_id && !validate_route(r)) result.push_back(a.activity_id);
    return result;
}
PlanExecution::PlanExecution(const Plan& plan) {
    if (!plan.plan_id || !plan.config_revision || plan.owners.empty() || plan.owners.size() > 64 || plan.steps.empty() || plan.steps.size() > 64) {
        status_ = TransitionStatus::aborted; return;
    }
    std::set<OctopusId> owners;
    for (const auto& owner : plan.owners) if (!owner || !owners.insert(owner).second) { status_ = TransitionStatus::aborted; return; }
    for (size_t i = 0; i < plan.steps.size(); ++i) {
        if (!plan.steps[i].binding_id || plan.steps[i].dependencies.size() > 32) { status_ = TransitionStatus::aborted; return; }
        for (auto dependency : plan.steps[i].dependencies) if (dependency >= i) { status_ = TransitionStatus::aborted; return; }
    }
    plan_ = plan; results_.assign(plan.steps.size(), StepResult::not_sent);
}
bool PlanExecution::prepared(OctopusId owner, bool accepted) {
    if (status_ != TransitionStatus::preparing || !contains(plan_.owners, owner)) return false;
    if (!accepted) { status_ = TransitionStatus::aborted; return true; }
    if (!contains(prepared_, owner)) prepared_.push_back(owner);
    if (prepared_.size() == plan_.owners.size()) status_ = TransitionStatus::fencing;
    return true;
}
bool PlanExecution::barrier_complete(OctopusId owner) {
    if (status_ != TransitionStatus::fencing || !contains(plan_.owners, owner)) return false;
    if (!contains(drained_, owner)) drained_.push_back(owner);
    if (drained_.size() == plan_.owners.size()) status_ = TransitionStatus::executing;
    return true;
}
std::vector<size_t> PlanExecution::ready_steps() const {
    std::vector<size_t> ready;
    if (status_ != TransitionStatus::executing) return ready;
    for (size_t i = 0; i < plan_.steps.size(); ++i) {
        if (results_[i] != StepResult::not_sent) continue;
        bool dependencies = true;
        for (auto dependency : plan_.steps[i].dependencies) {
            if (dependency >= results_.size()) { dependencies = false; break; }
            const auto outcome = results_[dependency];
            if (outcome != StepResult::observed && (outcome != StepResult::applied || plan_.steps[dependency].require_observation)) dependencies = false;
        }
        if (dependencies) ready.push_back(i);
    }
    return ready;
}
bool PlanExecution::record(size_t index, StepResult result) {
    if ((status_ != TransitionStatus::executing && status_ != TransitionStatus::verifying) || index >= results_.size()) return false;
    if (result == StepResult::not_sent) return false;
    const auto prior = results_[index];
    if (prior == StepResult::not_sent) {
        if (!contains(ready_steps(), index)) return false;
    } else if (prior == StepResult::observed || prior == StepResult::rejected || prior == StepResult::timed_out || prior == StepResult::unknown ||
        (prior == StepResult::applied && result != StepResult::observed)) return false;
    results_[index] = result;
    if (result == StepResult::rejected || result == StepResult::timed_out || result == StepResult::unknown) { status_ = TransitionStatus::degraded; return true; }
    bool all_applied = true, all_observed = true;
    for (size_t i = 0; i < results_.size(); ++i) {
        if (results_[i] != StepResult::applied && results_[i] != StepResult::observed) all_applied = false;
        if (plan_.steps[i].require_observation && results_[i] != StepResult::observed) all_observed = false;
    }
    if (all_applied) status_ = all_observed ? TransitionStatus::active : TransitionStatus::verifying;
    return true;
}
void PlanExecution::fail_before_effects() {
    if (std::all_of(results_.begin(), results_.end(), [](StepResult r) { return r == StepResult::not_sent; })) status_ = TransitionStatus::aborted;
    else status_ = TransitionStatus::degraded;
}
} // namespace amity::system
