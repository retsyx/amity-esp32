#pragma once

#include "amity/hub_service.hpp"
#include "cec/board_profile.hpp"
#include "amity/system/model.hpp"
#include "system_configuration.hpp"

namespace amity {
// Typed ingress shared by local and authenticated consortium adapters.
class SystemControl {
public:
    void set_identity(system::ConsortiumId consortium, system::OctopusId octopus) {
        consortium_ = consortium; octopus_ = octopus;
        local_.set_system_identity(consortium, octopus);
    }
    const system::ConsortiumId &consortium_id() const { return consortium_; }
    const system::OctopusId &octopus_id() const { return octopus_; }
    void set_boot_identity(system::BootId boot) { boot_id_ = boot; local_.set_boot_identity(boot); }
    system::BootId boot_identity() const { return boot_id_; }
    void set_hardware_error(std::string error) {
        if(error.size()>192)error.resize(192);
        std::atomic_store(&hardware_error_,std::make_shared<const std::string>(std::move(error)));
    }
    std::string hardware_error() const { const auto error=std::atomic_load(&hardware_error_);return error?*error:std::string{}; }
    void set_coordinator_authority(bool local_coordinator, uint64_t epoch) {
        const auto config=std::atomic_load(&active_config_);
        if(local_coordinator && (epoch!=1 || (config && (!config->controller_octopus_id || *config->controller_octopus_id!=octopus_))))local_coordinator=false;
        local_coordinator_.store(local_coordinator); local_.set_coordinator_authority(local_coordinator, epoch);
    }
    esp_err_t transition_coordinator_authority(bool local_coordinator, uint64_t epoch) {
        const auto config=std::atomic_load(&active_config_);
        if(local_coordinator && (epoch!=1 || (config && (!config->controller_octopus_id || *config->controller_octopus_id!=octopus_))))return ESP_ERR_INVALID_STATE;
        const auto result = local_.transition_coordinator_authority(local_coordinator, epoch);
        if (result == ESP_OK) local_coordinator_.store(local_coordinator);
        return result;
    }
    system::CecPortRef port(BusSide side) const {
        return {consortium_, octopus_, 0, side == BusSide::Front ? system::Side::front : system::Side::back};
    }
    esp_err_t start(int front, int back, uint16_t physical = 0x1000) {
        if (!consortium_ || !octopus_) return ESP_ERR_INVALID_STATE;
        return local_.start(front, back, physical);
    }
    esp_err_t start(const cec::BoardProfile& profile, uint16_t physical = 0x1000) {
        if (!consortium_ || !octopus_) return ESP_ERR_INVALID_STATE;
        const auto result = local_.start(profile, physical);
        if (result == ESP_OK) board_profile_ = profile;
        return result;
    }
    esp_err_t start(const cec::BoardProfile& profile, const std::array<uint16_t, amity::max_tentacles>& physical) {
        if (!consortium_ || !octopus_) return ESP_ERR_INVALID_STATE;
        const auto result = local_.start(profile, physical);
        if (result == ESP_OK) board_profile_ = profile;
        return result;
    }
    esp_err_t set_presence_reader(std::function<int(int)> read) { return local_.set_presence_reader(std::move(read)); }
    std::vector<AdapterRuntimeState> adapter_runtime_snapshot() const { return local_.adapter_runtime_snapshot(); }
    esp_err_t commit_adapter_settings(system::BootId boot, std::vector<std::pair<uint8_t, uint64_t>> presence,
                                     uint64_t deadline, std::function<esp_err_t()> persist) {
        return local_.commit_adapter_settings(boot, std::move(presence), deadline, std::move(persist));
    }
    uint8_t supported_tentacles() const { return board_profile_.tentacle_count; }
    bool connector_enabled(uint8_t id) const { return cec::connector_enabled(board_profile_, id); }
    std::optional<bool> connector_presence(uint8_t id) const { return local_.connector_presence(id); }
    uint8_t configured_tentacles_mask() const {
        uint8_t mask = 0;
        for (uint8_t id = 0; id < supported_tentacles(); ++id) if (connector_enabled(id)) mask |= uint8_t(1u << id);
        return mask;
    }
    bool configure(std::vector<Activity> activities, std::vector<Quirk> quirks = {}) {
        return local_.configure(std::move(activities), std::move(quirks));
    }
    esp_err_t configure_with_commit(std::vector<Activity> activities, std::vector<Quirk> quirks,
                                    std::function<esp_err_t()> commit) {
        return local_.configure_with_commit(std::move(activities), std::move(quirks), std::move(commit));
    }
    bool accepts_local_input() const;
    esp_err_t request_activity(int index);
    int activity_target() const { return local_.activity_target(); }
    bool select_activity(int index) { return request_activity(index) == ESP_OK; }
    bool press_key(uint8_t key, bool repeat = false);
    bool press_key_for_activity(uint8_t key, uint32_t generation, bool repeat = false) {
        return accepts_local_input() && local_.press_key_for_activity(key, generation, repeat);
    }
    uint32_t activity_generation() const { return local_.activity_generation(); }
    uint32_t activity_selection() const { return local_.activity_selection(); }
    bool release_key(uint8_t key = 0xff);
    bool standby() { return request_activity(-1) == ESP_OK; }
    esp_err_t request_remote_activity(unsigned button);
    esp_err_t request_remote_activity(unsigned button, uint32_t expected_generation);
    bool press_remote_key(uint8_t key, bool repeat = false);
    bool press_remote_key_for_activity(uint8_t key, uint32_t expected_generation, bool repeat = false);
    bool release_remote_key(uint8_t key = 0xff);
    esp_err_t repair_remote_activity(uint32_t expected_generation);
    esp_err_t stop_remote_zone();
    esp_err_t stop_remote_zone(uint32_t expected_generation);
    uint32_t remote_activity_generation() const;
    int remote_activity_target() const;
    bool discover() { return local_.discover(); }
    system::AdmissionResult discover_authorized(uint32_t ttl) { return local_.discover_authorized(ttl); }
    Snapshot snapshot() const { return local_.snapshot(); }
    void on_change(std::function<void(const Snapshot &)> callback) { local_.on_change(std::move(callback)); }
    esp_err_t configure_v2(config_store::ConfigurationV2, std::function<esp_err_t()> commit = {});
    esp_err_t request_activity_v2(system::ZoneId zone, system::ActivityId activity, system::MessageId request,
                                 std::optional<uint64_t> expected = {}, bool repair = false) {
        return accepts_local_input() ? local_.request_activity_v2(zone, activity, request, expected, repair) : ESP_ERR_INVALID_STATE;
    }
    esp_err_t stop_zone_v2(system::ZoneId zone) { return accepts_local_input() ? local_.stop_zone_v2(zone) : ESP_ERR_INVALID_STATE; }
    esp_err_t execute_authorized(system::AuthorizedCecOperation operation) { return local_.execute_authorized(std::move(operation)); }
    system::AdmissionResult admit_authorized(system::AuthorizedCecOperation operation, uint32_t timeout_ms = 1000) { return local_.admit_authorized(std::move(operation), timeout_ms); }
    std::optional<bool> remote_key_active(const system::AuthorizedCecOperation& operation, bool synchronously_admitted = false) const { return local_.remote_key_active(operation, synchronously_admitted); }
    esp_err_t configure_member_slice(config_store::ConfigurationV2, system::OctopusId coordinator, uint64_t epoch);
    // Passive OTA migration metadata; installs no policy or planner.
    esp_err_t configure_passive_metadata(const config_store::ConfigurationV2&);
    system::AdmissionResult prepare_member_configuration(config_store::ConfigurationV2, system::OctopusId coordinator, uint64_t epoch, std::string payload_hash);
    esp_err_t activate_member_configuration(const std::string& payload_hash);
    void abort_member_configuration(const std::string& payload_hash);
    system::AdmissionResult prepare_remote_plan(system::Plan plan, std::vector<system::PortPolicySnapshot> policies) { return local_.prepare_remote_plan(std::move(plan), std::move(policies)); }
    system::AdmissionResult fence_remote_route(system::SessionId session, uint64_t generation) { return local_.fence_remote_route(session, generation); }
    bool remote_route_quiescent(system::SessionId session, uint64_t generation) const { return local_.remote_route_quiescent(session, generation); }
    system::AdmissionResult activate_remote_plan(system::PlanId id, uint32_t ttl) { return local_.activate_remote_plan(id, ttl); }
    system::AdmissionResult abort_remote_plan(system::PlanId id) { return local_.abort_remote_plan(id); }
    system::AdmissionResult renew_remote_authority(system::PlanId id, system::SessionId session, uint64_t generation, uint32_t ttl) { return local_.renew_remote_authority(id, session, generation, ttl); }
    std::vector<system::PortPolicySnapshot> local_policy_snapshot() const { return local_.local_policy_snapshot(); }
    void set_remote_execution_hooks(system::RemoteExecutionHooks hooks) { local_.set_remote_execution_hooks(std::move(hooks)); }
    system::AdmissionResult update_remote_owner(system::OctopusId, std::vector<system::PortPolicySnapshot>, bool ready, uint32_t authority_ttl_ms = 5000);
    system::AdmissionResult remote_plan_result(system::OctopusId owner, system::PlanId id, system::RemotePlanPhase phase, bool accepted, std::string error = {}) { return local_.remote_plan_result(owner, id, phase, accepted, std::move(error)); }
    system::AdmissionResult remote_operation_result(system::OctopusId owner, system::CommandId id, system::StepResult result, std::string error = {}) { return local_.remote_operation_result(owner, id, result, std::move(error)); }
    system::InputContext input_context() const { return local_.input_context(); }
    system::AdmissionResult submit_input_intent(system::InputIntent input) { return accepts_local_input() ? local_.submit_input_intent(std::move(input)) : system::AdmissionResult{system::AdmissionCode::unauthorized, "Only the original Amity accepts user input"}; }
    esp_err_t retire_membership() { local_coordinator_.store(false); return local_.retire_membership(); }
    esp_err_t begin_maintenance() { maintenance_.store(true); return local_.begin_maintenance(); }
    void end_maintenance() { local_.end_maintenance(); maintenance_.store(false); }
    bool maintenance_ready() const { return local_.maintenance_ready(); }
    system::ExecutionSnapshot execution_snapshot() const { return local_.execution_snapshot(); }
    std::vector<system::CecObservation> native_observations() const { return local_.execution_snapshot().native_observations; }
    std::vector<system::RequestedActivity> suspended_requested_sessions() const { return local_.execution_snapshot().suspended_requested_sessions; }
    std::optional<system::ActivityOutcome> activity_outcome(system::MessageId request) const {
        for (const auto& outcome : local_.execution_snapshot().activity_outcomes) if (outcome.request_id == request) return outcome;
        return {};
    }
    std::optional<system::CommandOutcome> command_outcome(system::CommandId command) const {
        for (const auto& outcome : local_.execution_snapshot().command_outcomes) if (outcome.command_id == command) return outcome;
        return {};
    }
    std::optional<config_store::ConfigurationV2> active_configuration() const;
    bool schema2_active() const { return bool(std::atomic_load(&active_config_)); }
    std::optional<config_store::ConfiguredActivity> external_activity(unsigned) const;
    std::optional<config_store::ExternalSlot> external_slot(unsigned) const;
private:
    bool compile_v2(const config_store::ConfigurationV2&, system::LocalConfiguration&, std::string& error) const;
    esp_err_t send_remote_intent(system::InputIntent, std::optional<system::InputContext> captured = {});
    struct RemoteStamp { std::string fingerprint; uint32_t generation = 0; system::InputContext context; };
    std::shared_ptr<const RemoteStamp> remote_stamp() const;
    HubService local_;
    cec::BoardProfile board_profile_ = cec::legacy_board2_profile;
    system::ConsortiumId consortium_;
    system::OctopusId octopus_;
    system::BootId boot_id_;
    std::shared_ptr<const std::string> hardware_error_;
    std::shared_ptr<const config_store::ConfigurationV2> active_config_;
    struct PreparedMember { std::string payload_hash; config_store::ConfigurationV2 configuration; };
    std::shared_ptr<const PreparedMember> prepared_member_;
    std::atomic<bool> local_coordinator_{true};
    std::atomic<bool> maintenance_{false};
    struct RemoteHold { system::HoldToken token; system::ZoneId zone; system::InputContext context; };
    std::array<std::shared_ptr<const RemoteHold>, 256> remote_holds_{};
    mutable std::shared_ptr<const RemoteStamp> remote_stamp_;
};
}
