#pragma once
#include "amity/system/planner.hpp"
#include <memory>

namespace amity::system {
struct LocalActivity {
    ActivityId activity_id; ZoneId zone_id; std::string label;
    std::optional<ComponentId> source, display, audio;
};
struct LocalExternalSlot { uint16_t slot; ActivityId activity_id; bool hidden = false, tombstone = false; std::string alias; };
// Compiled nonsecret local slice. Persisted selectors stay in each binding's
// device_name; endpoint LA/status/revision are populated only by live discovery.
struct LocalConfiguration {
    Topology topology;
    std::vector<PortPolicySnapshot> port_templates;
    ZoneId primary_zone_id;
    std::vector<LocalActivity> activities;
    std::vector<LocalExternalSlot> external_slots;
    uint64_t presentation_revision = 0;
    std::string executable_hash;
    bool member = false;
    OctopusId coordinator_id;
    uint64_t coordinator_epoch = 0;
};
struct ActivityOutcome { MessageId request_id; std::optional<PlanId> plan_id; std::string stage, error; };
struct CommandOutcome { CommandId command_id; StepResult result = StepResult::admitted; std::string error; };
struct RequestedActivity { ZoneId zone_id; ActivityId activity_id; };
struct ExecutionSnapshot {
    uint64_t applied_config_revision = 0;
    uint64_t applied_presentation_revision = 0;
    std::string applied_policy_hash;
    uint64_t state_revision = 0;
    std::vector<Session> sessions;
    std::optional<PlanId> executing_plan;
    std::vector<StepResult> steps;
    std::string error;
    std::vector<ActivityOutcome> activity_outcomes;
    std::vector<CommandOutcome> command_outcomes;
    std::vector<CecObservation> native_observations;
    std::vector<RequestedActivity> suspended_requested_sessions;
};
enum class RemotePlanPhase { prepare, fence, activate };
struct InputContext { InputSessionId input_session_id; uint64_t state_revision = 0; };
enum class InputAction { select_activity, stop_zone, key_press, hold_release };
struct InputIntent {
    InputAction action = InputAction::select_activity;
    MessageId request_id;
    ZoneId zone_id;
    ActivityId activity_id;
    std::optional<ComponentId> component_id;
    uint8_t key = 0;
    HoldToken hold_token;
    bool repeat = false;
    InputSessionId input_session_id;
    uint64_t expected_state_revision = 0;
    uint64_t admissible_until_ms = 0; // Receiver-local causal deadline; never serialized as sender time.
};
// Hooks enqueue bounded authenticated protocol messages; they never wait for a
// network response on the CEC owner task. Results return through the methods below.
struct RemoteExecutionHooks {
    std::function<bool(OctopusId, const Plan&)> prepare;
    std::function<bool(OctopusId, const Plan&, const std::optional<Session>&)> fence;
    std::function<bool(OctopusId, const Plan&, uint32_t)> activate;
    std::function<bool(OctopusId, PlanId, const AuthorizedCecOperation&)> send;
    std::function<void(OctopusId, const Plan&)> abort;
};
// Single-owner live coordinator. A lane is an independent physical CEC port;
// no network, GPIO or blocking waits occur here. The adapter retains each work
// until its bounded wire completion and invalidates frame-start guards on fence.
class Coordinator {
public:
    explicit Coordinator(std::function<bool(uint8_t*, size_t)> secure_random);
    AdmissionResult install(Topology, std::vector<PortPolicySnapshot>, OctopusId, uint64_t now_ms);
    AdmissionResult refresh_bindings(Topology, std::vector<PortPolicySnapshot>, uint64_t now_ms);
    bool can_rebase_revision(uint64_t next_revision) const;
    bool rebase_revision(uint64_t next_revision);
    PlanningResult select(const SelectRequest&, uint64_t now_ms);
    bool stop(ZoneId, uint64_t now_ms);
    AdmissionResult key(ZoneId, ComponentId, uint8_t, bool held, HoldToken, uint64_t now_ms, uint64_t admissible_until_ms = 0);
    bool release(HoldToken, uint64_t now_ms = 0);
    AdmissionResult execute_authorized(const AuthorizedCecOperation&, uint64_t now_ms);
    AdmissionResult prepare_remote_plan(const Plan&, const std::vector<PortPolicySnapshot>&, uint64_t now_ms);
    AdmissionResult fence_remote_route(SessionId, uint64_t generation);
    bool remote_route_quiescent(SessionId, uint64_t generation) const;
    AdmissionResult activate_remote_plan(PlanId, uint32_t authority_ttl_ms, uint64_t now_ms);
    void abort_remote_plan(PlanId);
    AdmissionResult renew_remote_authority(PlanId, SessionId, uint64_t generation, uint32_t authority_ttl_ms, uint64_t now_ms);
    void remote_execution_hooks(RemoteExecutionHooks hooks) { remote_hooks_ = std::move(hooks); }
    AdmissionResult update_remote_owner(OctopusId, std::vector<PortPolicySnapshot>, bool ready, uint32_t authority_ttl_ms, uint64_t now_ms);
    bool remote_plan_result(OctopusId, PlanId, RemotePlanPhase, bool accepted, const std::string& error, uint64_t now_ms);
    bool remote_operation_result(OctopusId, CommandId, StepResult, const std::string& error, uint64_t now_ms);
    AdmissionResult renew_discovery_authority(uint32_t ttl_ms, uint64_t now_ms);
    void observe(const CecFrame&, uint64_t now_ms);
    void tick(uint64_t now_ms, bool allow_repeat = true);
    void set_wire_adapter(const CecPortRef&, WireSubmit, WireCancel);
    bool local_repeat_ready(uint64_t now_ms) const;
    bool local_repeat_in_flight() const;
    std::vector<AuthorizedCecOperation> active_key_contexts() const;
    void complete_frame(const CecPortRef&, uint64_t work_id, WireOutcome, uint64_t now_ms, bool started = true);
    ExecutionSnapshot snapshot() const;
    const Topology& topology() const { return topology_; }
    const std::vector<PortPolicySnapshot>& policies() const { return policies_; }
    bool quiescent() const;
    void quiesce();
private:
    struct Lane { CecPortRef port; LocalCecAgent agent; };
    struct WireAdapter { CecPortRef port; WireSubmit submit; WireCancel cancel; };
    std::vector<WireAdapter> wire_adapters_;
    struct StepCommand { CommandId command_id; size_t step; CecPortRef port; };
    struct Held { HoldToken token; AuthorizedCecOperation operation; };
    template<class T> T fresh() { return T::generate(random_).value_or(T{}); }
    Lane* lane(const CecPortRef&);
    AuthorizedCecOperation operation(const ComponentBinding&, const Session&, OperationKind, const OperationArguments&, uint64_t);
    bool install_grants(uint64_t now_ms);
    AdmissionResult check_policies(const std::vector<PortPolicySnapshot>&, uint64_t now_ms) const;
    AdmissionResult install_policies(const std::vector<PortPolicySnapshot>&, uint64_t now_ms);
    void fail(const std::string&);
    std::function<bool(uint8_t*, size_t)> random_;
    OctopusId local_;
    Topology topology_;
    std::vector<PortPolicySnapshot> policies_;
    std::vector<Lane> lanes_;
    std::unique_ptr<ResourcePlanner> planner_;
    std::optional<Plan> plan_;
    std::unique_ptr<PlanExecution> execution_;
    std::vector<StepCommand> commands_;
    std::vector<Held> held_;
    struct AuthorizedRecord { AuthorizedCecOperation operation; AdmissionResult result; CommandOutcome outcome; bool observed = false; };
    std::vector<AuthorizedRecord> authorized_;
    std::optional<Plan> remote_plan_;
    std::vector<Plan> active_remote_plans_;
    std::vector<PortPolicySnapshot> remote_policies_;
    std::vector<PortPolicySnapshot> remote_owner_policies_;
    struct RemoteProgress { OctopusId owner; bool fence_sent = false, fenced = false, activate_sent = false, activated = false; };
    std::vector<RemoteProgress> remote_progress_;
    RemoteExecutionHooks remote_hooks_;
    bool local_fenced_ = false, local_grants_installed_ = false;
    uint64_t plan_deadline_ = 0;
    std::string error_;
    uint64_t verify_deadline_ = 0;
    bool observed_evidence_ = false;
};
}
