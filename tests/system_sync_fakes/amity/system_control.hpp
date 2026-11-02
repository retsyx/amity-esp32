#pragma once
#include "system_configuration.hpp"
#include "amity/system/execution.hpp"
#include "amity/system/owned_intent.hpp"
#include "esp_err.h"
#include <functional>
#include <tuple>

namespace amity {
// A spy at the live-owner boundary. All synchronization, parsing and storage
// decisions under test come from the production synchronization/journal code.
class SystemControl {
public:
    config_store::Configuration configuration;
    system::OctopusId identity;
    system::BootId boot;
    bool local_input = true, local_scan_result = true, remote_scan_result = true, lookup_result = true, auto_apply = true, update_result = true, refresh_result = true, fence_result = true;
    system::ExecutionSnapshot applied;
    std::optional<bool> source_present = true;
    esp_err_t configure_result = ESP_OK, bind_result = ESP_OK;
    system::AdmissionCode admission = system::AdmissionCode::accepted;
    unsigned configure_calls = 0, bind_calls = 0, local_scan_calls = 0;
    system::OctopusId bound_controller;
    uint64_t bound_revision = 0;
    std::vector<system::OwnedIntent> submitted;
    std::vector<system::OctopusId> refreshed_context;
    std::vector<system::OwnedAuthorization> authorizations;
    std::vector<system::OwnedAdapterSnapshot> snapshots, updates;
    std::vector<system::CecObservation> observations;
    std::vector<std::pair<system::OctopusId,uint8_t>> remote_scans;
    std::vector<std::pair<system::OctopusId,system::LinkId>> fences;
    std::function<bool(const system::OwnedIntent&)> sender;
    std::function<void(const system::OwnedAdapterSnapshot&)> observer;
    std::function<bool(const system::OwnedAuthorization&)> authorizer;
    std::function<uint64_t(const system::OwnedAuthorization&)> deadline;

    system::ConsortiumId consortium_id() const { return configuration.consortium_id; }
    system::OctopusId octopus_id() const { return identity; }
    system::BootId boot_identity() const { return boot; }
    std::optional<config_store::Configuration> active_configuration() const { return configuration; }
    bool accepts_local_input() const { return local_input; }
    void set_owned_sender(std::function<bool(const system::OwnedIntent&)> fn) { sender=std::move(fn); }
    void set_owned_observer(std::function<void(const system::OwnedAdapterSnapshot&)> fn) { observer=std::move(fn); }
    void set_owned_authorizer(std::function<bool(const system::OwnedAuthorization&)> fn) { authorizer=std::move(fn); }
    void set_owned_deadline(std::function<uint64_t(const system::OwnedAuthorization&)> fn) { deadline=std::move(fn); }
    system::ExecutionSnapshot execution_snapshot() const { return applied; }
    uint64_t executing_configuration_revision()const{return applied.applied_config_revision;}
    system::AdmissionResult submit_owned(const system::OwnedIntent& intent,const system::OwnedAuthorization& auth) {
        submitted.push_back(intent);authorizations.push_back(auth);
        if(authorizer&&!authorizer(auth))return {system::AdmissionCode::unauthorized,"injected runtime authorization rejection"};
        system::AdmissionResult result{admission,{}};
        if(admission==system::AdmissionCode::accepted||admission==system::AdmissionCode::unresolved||admission==system::AdmissionCode::overloaded)result.source_present=source_present;
        return result;
    }
    esp_err_t bind_owned_controller(system::OctopusId controller,uint64_t revision) {
        ++bind_calls;
        if(!configuration.controller_octopus_id||*configuration.controller_octopus_id!=controller)return ESP_ERR_INVALID_STATE;
        if(bind_result!=ESP_OK)return bind_result;
        bound_controller=controller;bound_revision=revision;return ESP_OK;
    }
    bool update_owned_snapshot(system::OwnedAdapterSnapshot report) { updates.push_back(std::move(report));return update_result; }
    bool refresh_owned_context(system::OctopusId owner,uint64_t,std::function<void(system::OwnedContextResult)>) { refreshed_context.push_back(owner);return refresh_result; }
    std::vector<system::OwnedAdapterSnapshot> owned_snapshots() const { return snapshots; }
    std::vector<system::CecObservation> native_observations() const { return observations; }
    bool fence_owned(system::OctopusId owner,system::LinkId link) { fences.push_back({owner,link});return fence_result; }
    bool discover() { ++local_scan_calls;return local_scan_result; }
    bool discover_owned(system::OctopusId owner,uint8_t adapter) {
        remote_scans.push_back({owner,adapter});return remote_scan_result;
    }
    esp_err_t configure(config_store::Configuration next,std::function<esp_err_t()> commit={}) {
        ++configure_calls;
        if(configure_result!=ESP_OK)return configure_result;
        const auto result=commit?commit():ESP_OK;
        if(result==ESP_OK){
            configuration=std::move(next);
            if(auto_apply){applied.applied_config_revision=configuration.config_revision;applied.applied_presentation_revision=configuration.presentation_revision;}
        }
        return result;
    }
};
}
