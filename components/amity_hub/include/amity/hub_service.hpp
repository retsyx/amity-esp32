// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/hub.hpp"
#include "amity/system/execution.hpp"
#include "cec/board_profile.hpp"
#include "esp_err.h"
#include <array>
#include <functional>
#include <memory>
namespace amity {
struct AdapterRuntimeState {
    uint8_t tentacle_id=0;bool configured=false;std::optional<bool> present;
    uint64_t presence_generation=1;std::optional<uint16_t> observed_front_physical,effective_front_physical;
};
// One owner task advances every original per-adapter discovery/key executor.
class HubService {
public:
    HubService();~HubService();
    esp_err_t start(int front,int back,uint16_t physical=0x1000);
    esp_err_t start(const cec::BoardProfile&,uint16_t physical=0x1000);
    esp_err_t start(const cec::BoardProfile&,const std::array<uint16_t,amity::max_tentacles>&);
    esp_err_t set_presence_reader(std::function<int(int)>);
    std::optional<bool> connector_presence(uint8_t)const;
    std::vector<AdapterRuntimeState> adapter_runtime_snapshot()const;
    esp_err_t commit_adapter_settings(system::BootId,std::vector<std::pair<uint8_t,uint64_t>>,uint64_t,std::function<esp_err_t()>);
    bool configure(std::vector<Activity>);
    esp_err_t configure_with_commit(std::vector<Activity>,std::function<esp_err_t()>);
    esp_err_t request_activity(int,uint32_t* accepted_selection=nullptr);esp_err_t repair_activity();uint32_t activity_selection()const;int activity_target()const;
    bool select_activity(int);bool press_key(uint8_t,bool repeat=false);bool press_key_for_activity(uint8_t,uint32_t,bool repeat=false);
    uint32_t activity_generation()const;bool release_key(uint8_t);bool release_key();bool standby();bool discover();
    Snapshot snapshot()const;void on_change(std::function<void(const Snapshot&)>);
    void set_system_identity(system::ConsortiumId,system::OctopusId);void set_boot_identity(system::BootId);
    void set_coordinator_authority(bool,uint64_t);esp_err_t transition_coordinator_authority(bool,uint64_t);
    esp_err_t retire_membership();esp_err_t begin_maintenance();void end_maintenance();bool maintenance_ready()const;
    using OwnedSender=Hub::OwnedSender;
    esp_err_t configure_scoped(std::vector<Activity>,std::vector<system::ScopedSelector>,OwnedSender,
        system::OctopusId controller,uint64_t config_revision,std::function<esp_err_t()> commit={},uint64_t presentation_revision=1);
    system::AdmissionResult submit_owned(const system::OwnedIntent&,const system::OwnedAuthorization&,uint32_t timeout_ms=1000);
    esp_err_t bind_owned_controller(system::OctopusId,uint64_t config_revision);
    void set_owned_authorizer(std::function<bool(const system::OwnedAuthorization&)>);
    void set_owned_deadline(std::function<uint64_t(const system::OwnedAuthorization&)>);
    void set_owned_observer(std::function<void(const system::OwnedAdapterSnapshot&)>);
    void set_owned_input_observer(std::function<void(const system::OwnedIntent&,const system::OwnedAuthorization&,const system::AdmissionResult&)>);
    bool update_owned_snapshot(system::OwnedAdapterSnapshot);
    bool refresh_owned_context(system::OctopusId);
    std::vector<system::OwnedAdapterSnapshot> owned_snapshots()const;
    bool fence_owned(system::OctopusId controller,system::LinkId link);
    bool discover_owned(system::OctopusId owner,uint8_t adapter);
    uint64_t applied_configuration_revision()const;
    bool owned_input_result(system::OctopusId,uint64_t generation,uint64_t sequence,bool source_present);
private:
    esp_err_t request_activity(int,bool notify_selection,uint32_t* accepted_selection);struct Impl;std::unique_ptr<Impl> impl_;
};
}
