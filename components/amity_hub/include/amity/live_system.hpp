#pragma once
#include "amity/hub.hpp"
#include "amity/system/coordinator.hpp"
#include "cec/bus.hpp"
#include <atomic>

namespace amity {
// Sole HubService owner-task adapter. Every actual wire is a distinct lane;
// planner, scanner and watchdog advance without waiting on another CEC line.
class LiveSystem {
public:
#ifdef ESP_PLATFORM
    static void* operator new(size_t);
    static void operator delete(void*);
#endif
    LiveSystem(system::ConsortiumId, system::OctopusId, system::BootId,
               std::function<bool(uint8_t*, size_t)> random, uint64_t coordinator_epoch = 1);
    void attach(system::CecPortRef, cec::Bus&, uint8_t identity, uint16_t physical);
    // Called only for an explicitly configured physical presence detector.
    // CEC silence is never converted into a physical removal event.
    void set_tentacle_present(uint8_t tentacle_id, bool present, uint64_t now_ms);
    // Observed native PA and selected startup address (observed, else this
    // connector's default). Selected fallback is never qualified route evidence.
    std::pair<std::optional<uint16_t>, std::optional<uint16_t>> front_physical_addresses(uint8_t) const;
    bool configure(system::LocalConfiguration, uint64_t now_ms, std::string& error);
    bool preserves_configuration(const system::LocalConfiguration&) const;
    bool revision_rebase_ready(const system::LocalConfiguration&) const;
    void fence_revision_work();
    bool select_slot(int, uint64_t now_ms, std::string& error);
    bool select(system::ZoneId, system::ActivityId, system::MessageId, std::optional<uint64_t>, uint64_t now_ms, std::string& error, uint64_t admissible_until_ms = 0);
    bool stop(system::ZoneId, uint64_t now_ms);
    system::AdmissionResult execute_authorized(const system::AuthorizedCecOperation&, uint64_t now_ms);
    system::AdmissionResult prepare_remote_plan(const system::Plan&, const std::vector<system::PortPolicySnapshot>&, uint64_t now_ms);
    system::AdmissionResult fence_remote_route(system::SessionId, uint64_t generation);
    bool remote_route_quiescent(system::SessionId, uint64_t generation) const;
    system::AdmissionResult activate_remote_plan(system::PlanId, uint32_t authority_ttl_ms, uint64_t now_ms);
    system::AdmissionResult renew_remote_authority(system::PlanId, system::SessionId, uint64_t generation, uint32_t authority_ttl_ms, uint64_t now_ms);
    void abort_remote_plan(system::PlanId);
    void remote_execution_hooks(system::RemoteExecutionHooks hooks) { coordinator_.remote_execution_hooks(std::move(hooks)); }
    system::AdmissionResult update_remote_owner(system::OctopusId owner, std::vector<system::PortPolicySnapshot> policies, bool ready, uint32_t ttl, uint64_t now) { return coordinator_.update_remote_owner(owner, std::move(policies), ready, ttl, now); }
    bool remote_plan_result(system::OctopusId owner, system::PlanId id, system::RemotePlanPhase phase, bool accepted, const std::string& error, uint64_t now) { return coordinator_.remote_plan_result(owner, id, phase, accepted, error, now); }
    bool remote_operation_result(system::OctopusId owner, system::CommandId id, system::StepResult result, const std::string& error, uint64_t now) { return coordinator_.remote_operation_result(owner, id, result, error, now); }
    // Includes unavailable binding diagnostics; executable agents retain only
    // verified permissions. Missing optional equipment must not block sync.
    std::vector<system::PortPolicySnapshot> local_policy_snapshot() const;
    std::vector<system::AuthorizedCecOperation> active_key_contexts() const { return coordinator_.active_key_contexts(); }
    bool key(uint8_t, bool repeat, uint64_t now_ms);
    bool release(uint64_t now_ms = 0);
    bool release_key(uint8_t key, uint64_t now_ms = 0);
    system::AdmissionResult input(const system::InputIntent&, uint64_t now_ms);
    void discover(uint64_t now_ms);
    system::AdmissionResult discover_authorized(uint32_t ttl_ms, uint64_t now_ms);
    void tick(uint64_t now_ms);
    bool local_repeat_ready(uint64_t now_ms) const { return coordinator_.local_repeat_ready(now_ms); }
    bool local_repeat_in_flight() const { return coordinator_.local_repeat_in_flight(); }
    Snapshot snapshot(const Snapshot& legacy) const;
    system::ExecutionSnapshot execution_snapshot() const;
    bool configured() const { return configured_; }
    bool quiescent() const {
        return coordinator_.quiescent() && std::none_of(ports_.begin(),ports_.end(),[](const auto& p){
            return std::any_of(p.submitted.begin(),p.submitted.end(),[](const auto& work){return work.active;});
        });
    }
    void quiesce();
    void maintenance(bool paused) { paused_ = paused; if (paused) quiesce(); }
    void fault(const std::string&);
private:
    struct Port {
        system::CecPortRef key;
        cec::Bus* bus;
        uint8_t identity;
        uint16_t physical;
        std::atomic<uint32_t>* wire_generation;
        system::ScanId scan, verified_scan;
        std::optional<system::DiscoveryProbe> probe;
        bool wire = false;
        uint32_t token = 0;
        uint64_t sequence = 0;
        bool authority_expired = false;
        bool reclaim_requested = false, reclaim_poll = false, reclaim_ready = false;
        uint8_t reclaim_vacant_polls = 0;
        bool present = true, claiming = false, allocation_probe = false, scan_driver = false;
        uint8_t claim_index = 0, vacant_polls = 0, claim_address = 15;
        uint64_t next_claim_ms = 0;
        std::shared_ptr<const system::DiscoverySnapshot> verified{};
        size_t slot_index = 0;
        struct Submitted { uint64_t work_id = 0; uint32_t token = 0; bool active = false; };
        std::array<Submitted, 9> submitted{}; // Eight messages plus caller-authorized final release overflow.
    };
    struct BindingGuard { system::BindingId id; uint64_t revision = 0; std::atomic<uint32_t> generation{0}; };
    template<class T> T fresh() { return T::generate(random_).value_or(T{}); }
    Port* port(const system::CecPortRef&);
    void observe(Port&, const cec::ReceiveObservation&, uint64_t);
    bool resolve(uint64_t, bool initial, std::string&);
    bool identity_available(Port&);
    system::AdmissionResult advance_remote_selection(uint64_t now_ms);
    void service_port(Port&, uint64_t);
    bool submit_wire(Port&, const system::WireWork*, size_t);
    void cancel_wire(Port&, uint64_t work_id);
    std::function<bool(uint8_t*, size_t)> random_;
    system::ConsortiumId consortium_;
    system::OctopusId octopus_;
    system::BootId boot_;
    uint64_t epoch_;
    uint64_t last_tick_ms_ = 0, next_discovery_ms_ = 0;
    system::LocalConfiguration config_;
    system::Coordinator coordinator_;
    system::DiscoveryService discovery_;
    std::vector<Port> ports_;
    // Owned by the containing internal-RAM HubService::Impl, never PSRAM.
    std::atomic<uint32_t> wire_generations_[10]{};
    std::atomic<uint32_t> work_generations_[10][9]{};
    BindingGuard binding_guards_[128];
    size_t guard_count_ = 0;
    std::vector<system::CecObservation> observations_;
    std::vector<system::ComponentBinding> bindings_;
    std::array<std::optional<system::HoldToken>, 256> held_{};
    struct RemoteSelection {
        system::InputIntent intent;
        std::vector<system::Session> original_sessions;
        std::vector<system::ZoneId> stopping;
        uint64_t config_revision = 0, presentation_revision = 0;
    };
    std::optional<RemoteSelection> remote_selection_;
    int selected_slot_ = -1;
    uint32_t next_token_ = 1, activity_generation_ = 1, discovery_generation_ = 0;
    bool configured_ = false, installed_ = false, resolving_ = false, preflight_ = false, paused_ = false;
    std::vector<system::SelectRequest> requests_;
    std::vector<system::ActivityOutcome> activity_outcomes_;
    std::vector<system::RequestedActivity> suspended_requests_;
    std::string error_;
};
}
