#pragma once
#include "amity/system/policy.hpp"

namespace amity::system {

enum class ResourceCompatibility { same_mode, exclusive, control_owner };
struct Resource {
    ResourceId resource_id;
    OctopusId owner_octopus_id;
    ResourceCompatibility compatibility = ResourceCompatibility::same_mode;
    std::vector<std::string> supported_modes;
};
struct ResourceDemand { ResourceId resource_id; std::string mode; OctopusId control_owner; };
enum class SignalDirection { input, output };
struct SignalPort {
    ComponentId component_id;
    uint8_t port_index = 0;
    SignalDirection direction = SignalDirection::input;
    friend bool operator==(const SignalPort& a, const SignalPort& b) {
        return a.component_id == b.component_id && a.port_index == b.port_index && a.direction == b.direction;
    }
};
enum class SignalEdgeKind { hdmi_cable, internal_equipment_path };
struct SignalEdge { SignalPort from, to; SignalEdgeKind kind; Support support = Support::unknown; };
struct PlanStep {
    BindingId binding_id;
    OperationKind operation = OperationKind::query_state;
    OperationArguments arguments;
    std::vector<size_t> dependencies;
    bool require_observation = false;
};
struct RouteDefinition {
    RouteId route_id;
    ComponentId source, display;
    std::vector<SignalPort> physical_path;
    std::vector<ResourceDemand> demands;
    std::vector<BindingId> required_bindings;
    std::vector<PlanStep> steps;
    bool requires_return_audio = false;
    Support return_audio_support = Support::unknown;
    bool changes_physical_addresses = false;
    bool qualified_physical_address_stability = false;
};
struct ActivityDefinition { ActivityId activity_id; ZoneId zone_id; RouteId route_id; };
struct Topology {
    uint64_t config_revision = 0;
    std::vector<Component> components;
    std::vector<ComponentBinding> bindings;
    std::vector<Resource> resources;
    std::vector<SignalEdge> signal_edges;
    std::vector<RouteDefinition> routes;
    std::vector<ActivityDefinition> activities;
    std::vector<OctopusId> ready_owners;
};
enum class SessionStatus { preparing, executing, verifying, active, degraded, failed, suspended_after_restart };
struct Session {
    SessionId session_id;
    ActivityId activity_id;
    ZoneId zone_id;
    RouteId route_id;
    PlanId plan_id;
    uint64_t route_generation = 0;
    SessionStatus status = SessionStatus::preparing;
    std::vector<ResourceDemand> demands;
};
struct SelectRequest {
    MessageId request_id;
    ZoneId zone_id;
    ActivityId activity_id;
    SessionId session_id;
    PlanId plan_id;
    std::optional<uint64_t> expected_state_revision;
    uint64_t admissible_until_ms = 0;
};
struct Plan {
    PlanId plan_id;
    uint64_t config_revision = 0;
    Session proposed_session;
    std::optional<SessionId> replaces_session;
    std::vector<PlanStep> steps;
    std::vector<OctopusId> owners;
    bool requires_topology_revalidation = false;
    uint64_t execution_expires_at_ms = 0; // Coordinator-local inherited intent deadline.
};
enum class PlanningStatus { proposed, conflict, unresolved, unsupported, stale, overloaded, duplicate };
struct PlanningResult {
    PlanningStatus status = PlanningStatus::unresolved;
    std::optional<Plan> plan;
    std::vector<SessionId> conflicting_sessions;
    std::string reason;
};
class ResourcePlanner {
public:
    explicit ResourcePlanner(const Topology&, size_t capacity = 64);
    PlanningResult select_activity(const SelectRequest&);
    bool commit_plan(PlanId, bool sufficient_observed_evidence);
    bool abort_plan(PlanId);
    std::vector<ResourceId> stop_session(SessionId);
    void suspend_after_restart();
    bool update_bindings(const Topology&);
    bool can_rebase_revision(uint64_t expected_revision, uint64_t next_revision) const;
    bool rebase_revision(uint64_t expected_revision, uint64_t next_revision);
    const std::vector<Session>& sessions() const { return sessions_; }
    uint64_t state_revision() const { return state_revision_; }
    // These are validated suggestions only; computing them never reserves or acts.
    std::vector<ActivityId> suggested_activities(ZoneId) const;
private:
    std::optional<std::string> validate_route(const RouteDefinition&) const;
    bool compatible(const ResourceDemand&, const ResourceDemand&) const;
    Topology topology_;
    size_t capacity_;
    uint64_t state_revision_ = 1;
    std::vector<Session> sessions_;
    std::vector<Plan> prepared_;
    struct Cached { SelectRequest request; PlanningResult result; };
    std::vector<Cached> requests_;
    bool valid_topology_ = false;
};

enum class TransitionStatus { preparing, fencing, executing, verifying, active, degraded, aborted };
enum class StepResult { not_sent, admitted, applied, observed, rejected, timed_out, unknown };
// Pure dependency/barrier state machine; callers dispatch only returned indices.
class PlanExecution {
public:
    explicit PlanExecution(const Plan&);
    bool prepared(OctopusId, bool accepted);
    bool barrier_complete(OctopusId);
    std::vector<size_t> ready_steps() const;
    bool record(size_t, StepResult);
    void fail_before_effects();
    TransitionStatus status() const { return status_; }
    const std::vector<StepResult>& step_results() const { return results_; }
private:
    Plan plan_;
    TransitionStatus status_ = TransitionStatus::preparing;
    std::vector<OctopusId> prepared_, drained_;
    std::vector<StepResult> results_;
};

} // namespace amity::system
