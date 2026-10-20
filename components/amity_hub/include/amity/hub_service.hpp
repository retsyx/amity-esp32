/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include "amity/hub.hpp"
#include "amity/system/coordinator.hpp"
#include "cec/board_profile.hpp"
#include "esp_err.h"
#include <functional>
#include <memory>
#include <array>

namespace amity {
struct AdapterRuntimeState {
    uint8_t tentacle_id = 0;
    bool configured = false;
    std::optional<bool> present;
    uint64_t presence_generation = 1;
    std::optional<uint16_t> observed_front_physical, effective_front_physical;
};
// Thread-safe facade. Commands are queued; snapshot returns the latest state.
// Callback runs on the hub task, outside the snapshot lock. It should be brief.
class HubService {
public:
    HubService();
    ~HubService();
    // Discover the front physical address on startup; use the supplied default
    // only if the back scan yields no usable physical-address report.
    esp_err_t start(int front_gpio, int back_gpio, uint16_t front_physical = 0x1000);
    // Board-local connector capacity and wiring. Unassigned GPIO pairs are
    // disabled; optional cable/module presence never follows CEC silence.
    esp_err_t start(const cec::BoardProfile&, uint16_t front_physical = 0x1000);
    esp_err_t start(const cec::BoardProfile&, const std::array<uint16_t, 5>& front_physical);
    // Optional board I/O adapter, installed before start. Only explicitly
    // declared presence GPIOs are sampled; a negative result means unknown.
    esp_err_t set_presence_reader(std::function<int(int)>);
    std::optional<bool> connector_presence(uint8_t tentacle_id) const;
    std::vector<AdapterRuntimeState> adapter_runtime_snapshot() const;
    // Persist only after idle admission, physical-generation validation and
    // owner quiescence. Own the callback/captures; success stays closed for reboot.
    esp_err_t commit_adapter_settings(system::BootId, std::vector<std::pair<uint8_t, uint64_t>>,
                                     uint64_t deadline_ms, std::function<esp_err_t()> persist);
    bool configure(std::vector<Activity> activities, std::vector<Quirk> quirks = {});
    // Reserve command capacity before committing external state. The callback
    // runs synchronously on the caller, must return an error on failure, and
    // must not call configure recursively. A failed reservation never calls it.
    esp_err_t configure_with_commit(std::vector<Activity> activities, std::vector<Quirk> quirks,
                                    std::function<esp_err_t()> commit);
    // ESP_ERR_TIMEOUT means ignored during settling; same-target repair bypasses it.
    esp_err_t request_activity(int index);
    int activity_target() const;
    bool select_activity(int index);
    // repeat=true holds the key: each completed wire attempt permits the next
    // press, until release or an activity/configuration change cancels it.
    bool press_key(uint8_t key, bool repeat = false);
    // The worker discards this press if the activity/configuration changed
    // after the originating operation captured its generation. repeat=true
    // starts a held stream with the same cancellation policy as press_key().
    bool press_key_for_activity(uint8_t key, uint32_t activity_generation, bool repeat = false);
    uint32_t activity_generation() const;
    bool release_key();
    bool standby();
    bool discover();
    Snapshot snapshot() const;
    void on_change(std::function<void(const Snapshot &)> callback);
    void set_system_identity(system::ConsortiumId, system::OctopusId);
    void set_boot_identity(system::BootId);
    void set_coordinator_authority(bool local_coordinator, uint64_t epoch);
    esp_err_t transition_coordinator_authority(bool local_coordinator, uint64_t epoch);
    esp_err_t configure_v2(system::LocalConfiguration, std::function<esp_err_t()> commit = {});
    esp_err_t request_activity_v2(system::ZoneId, system::ActivityId, system::MessageId,
                                 std::optional<uint64_t> expected_state = {}, bool repair = false);
    esp_err_t stop_zone_v2(system::ZoneId);
    esp_err_t execute_authorized(system::AuthorizedCecOperation);
    // Authenticated network consumers wait for validation by the sole owner;
    // queue admission is never reported as semantic authorization.
    system::AdmissionResult admit_authorized(system::AuthorizedCecOperation, uint32_t timeout_ms = 1000);
    // Absence proves inactive only for a caller retaining the exact down's
    // successful synchronous admission (published before its reply).
    std::optional<bool> remote_key_active(const system::AuthorizedCecOperation&, bool synchronously_admitted = false) const;
    esp_err_t configure_member_v2(system::LocalConfiguration, std::function<esp_err_t()> persist = {});
    system::AdmissionResult prepare_member_v2(system::LocalConfiguration, std::string payload_hash, std::function<esp_err_t()> persist);
    esp_err_t activate_member_v2(std::string payload_hash);
    void abort_member_v2(std::string payload_hash);
    system::AdmissionResult prepare_remote_plan(system::Plan, std::vector<system::PortPolicySnapshot>);
    system::AdmissionResult fence_remote_route(system::SessionId, uint64_t generation);
    bool remote_route_quiescent(system::SessionId, uint64_t generation) const;
    system::AdmissionResult activate_remote_plan(system::PlanId, uint32_t authority_ttl_ms);
    system::AdmissionResult abort_remote_plan(system::PlanId);
    system::AdmissionResult renew_remote_authority(system::PlanId, system::SessionId, uint64_t generation, uint32_t authority_ttl_ms);
    system::AdmissionResult discover_authorized(uint32_t ttl_ms);
    std::vector<system::PortPolicySnapshot> local_policy_snapshot() const;
    void set_remote_execution_hooks(system::RemoteExecutionHooks);
    system::AdmissionResult update_remote_owner(system::OctopusId, std::vector<system::PortPolicySnapshot>, bool ready, uint32_t authority_ttl_ms = 5000);
    system::AdmissionResult remote_plan_result(system::OctopusId, system::PlanId, system::RemotePlanPhase, bool accepted, std::string error = {});
    system::AdmissionResult remote_operation_result(system::OctopusId, system::CommandId, system::StepResult, std::string error = {});
    system::InputContext input_context() const;
    system::AdmissionResult submit_input_intent(system::InputIntent);
    esp_err_t retire_membership();
    esp_err_t begin_maintenance();
    void end_maintenance();
    bool maintenance_ready() const;
    system::ExecutionSnapshot execution_snapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
