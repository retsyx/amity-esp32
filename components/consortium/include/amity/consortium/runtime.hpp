// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/consortium/session.hpp"
#include "amity/system/owned_intent.hpp"
#include "esp_err.h"
#include <functional>
namespace amity::consortium {
struct RuntimeStatus {
    bool initialized=false,credential_ready=false,crypto_ready=false,coordinator=false,member=false;
    bool bootstrap_ready=false,enrollment_open=false,connected=false,synchronized=false;
    bool authority_ready=false;
    uint32_t active_peers=0;uint64_t epoch=0,membership_revision=0;
    std::string phase,last_error,leaf_spki_sha256;
};
struct RuntimeStartup {
    enum Role {unclaimed,coordinator,member,prepared,retired,recovery_required};
    Role role=unclaimed;model::ConsortiumId consortium_id;model::OctopusId authority_id;
    uint64_t epoch=0,membership_revision=0;
    bool passive()const{return role==member||role==prepared||role==retired||role==recovery_required;}
};
// NVS-only startup read before CEC/config; never generates keys or membership.
esp_err_t runtime_read_startup(model::OctopusId,RuntimeStartup &);
// Before runtime_start: GPIO/profile availability, not physical adapter presence.
esp_err_t runtime_set_hardware_profile(uint8_t supported_tentacles,uint8_t configured_tentacles_mask);
// Call once after the shared CHIP host and station networking exist. Credentials
// are stored in reset-owned amity_group; factory/bootstrap verifiers are retained
// separately. No listener opens until a durable original claim or committed join.
esp_err_t runtime_start(model::ConsortiumId,model::OctopusId,model::BootId,uint64_t config_revision);
RuntimeStatus runtime_status();
void runtime_configuration_changed(uint64_t config_revision,const std::string &policy_hash);
// Application-only claim after committed Matter commissioning and usable Wi-Fi
// (or proven retained original). A prepared member decision wins the durable CAS.
esp_err_t runtime_claim_original_controller();
// Role eligibility for physical Matter setup, independent of cosmetic phase.
bool runtime_original_setup_eligible();
// Called only by a physically authorized setup action; never an HTTP password alone.
esp_err_t runtime_open_enrollment(uint64_t radio_generation);
void runtime_close_enrollment();
// Cancels only transient radio work; prepared/committed journals are retained.
bool runtime_cancel_radio_workflows();
// Register before esp_matter::start. No second NimBLE/provisioning host is started.
esp_err_t runtime_register_gatt();
// Authenticated coordinator administration begins one bounded candidate workflow.
// Code encoding: amityc1:<OctopusId>:<32 lowercase hexadecimal secret bytes>.
esp_err_t runtime_add_octopus(const std::string &code,const std::string &hostname);
// Candidate-specific terminal bootstrap error; unrelated link errors are excluded.
std::string runtime_enrollment_error(model::OctopusId);
esp_err_t runtime_remove_octopus(model::OctopusId,model::MessageId request_id,uint64_t expected_membership_revision,std::optional<model::BootId> expected_target_boot);
// Complete means local roster removal. scheduled confirms only a durable reset marker.
std::string runtime_removal_operation(model::OctopusId,model::MessageId request_id);
bool runtime_removed_device_reset_authorized(model::OctopusId authenticated_coordinator);
// Radio bridge supplied by application's shared-host admission adapter.
struct RuntimeRadioHooks {
    std::function<esp_err_t(bool inviter,uint64_t &generation)> reserve;
    std::function<void(uint64_t generation)> release;
};
void runtime_radio_hooks(RuntimeRadioHooks);
struct RuntimeNetworkHooks {
    std::function<bool(std::string &ssid,std::string &password)> export_credentials;
    std::function<esp_err_t(const std::string &ssid,const std::string &password)> stage_connect;
    std::function<esp_err_t()> commit;
    std::function<void()> rollback;
    std::function<esp_err_t(model::JoinId,const std::string&,const std::string&,const std::string&)> stage_transaction;
    std::function<esp_err_t(model::JoinId,const std::string&)> commit_transaction;
    std::function<esp_err_t(model::JoinId,const std::string&)> finish_transaction;
    std::function<esp_err_t(model::JoinId,const std::string&,bool)> recover_transaction;
    std::function<esp_err_t(model::JoinId,const std::string&)> abort_transaction;
    std::function<esp_err_t(const std::string&)> set_hostname;
    std::function<std::string()> get_hostname;
};
void runtime_network_hooks(RuntimeNetworkHooks);
struct RuntimeControlHooks {
    std::function<std::string(model::OctopusId)> snapshot;
    std::function<Error(model::OctopusId,const Json &)> synchronize;
    // Exact receiver-local deadline and immutable authenticated TLS link.
    std::function<Error(model::OctopusId,const Envelope &,uint64_t,model::LinkId)> control;
    std::function<bool(model::OctopusId,model::BootId,model::LinkId)> disconnected;
    // Queue current nonemitting context before the first positive owner action.
    std::function<bool(model::OctopusId)> ready = {};
};
void runtime_control_hooks(RuntimeControlHooks);
struct RuntimeLifecycleHooks {
    std::function<esp_err_t()> retire;
    std::function<void(model::OctopusId)> revoked;
    std::function<esp_err_t(model::OctopusId authenticated_coordinator)> factory_reset;
    std::function<void()> after_factory_reset;
};
void runtime_lifecycle_hooks(RuntimeLifecycleHooks);
// Resume an already physically authorized reclaim before identity/CEC startup.
esp_err_t runtime_finish_reclaim(std::function<esp_err_t()> preserve_network = {});
// Physical USB only. Preserves origin/bootstrap and unrelated pairings/settings;
// rotates operational credentials and group identity via a resumable reboot.
esp_err_t runtime_reclaim_membership(model::OctopusId confirmed_local_id);

esp_err_t runtime_send_control(model::OctopusId,Kind,std::string payload,uint32_t ttl_ms=1000);
struct RuntimeSettingsHooks {
    std::function<std::string()> snapshot;
    std::function<Error(model::MessageId,const Json&,uint64_t receiver_deadline_ms,std::string& response)> apply;
    std::function<void()> after_apply;
};
void runtime_settings_hooks(RuntimeSettingsHooks);
std::string runtime_device_settings(model::OctopusId owner);
esp_err_t runtime_write_device_settings(model::OctopusId owner,model::MessageId,const Json& core,uint64_t admitted_until_ms=0);
std::string runtime_device_settings_operation(model::OctopusId owner,model::MessageId);
// Owner service rechecks this immediately before persisting; never renews TTL.
bool runtime_device_settings_authorized(model::MessageId);
// Local HTTP adapter calls after attempting its applied response, even if delivery fails.
void runtime_device_settings_response_attempted(model::OctopusId owner,model::MessageId);
struct RuntimeUpdateHooks {
    std::function<Error(model::OctopusId,const Envelope &)> handle;
    std::function<Error(model::OctopusId,const Json &)> result;
    // Match this accepted begin envelope ID before aborting local staging.
    std::function<void(model::OctopusId,model::MessageId)> disconnected = {};
};
void runtime_update_hooks(RuntimeUpdateHooks);
// Only the current coordinator may send requests; members may send results.
// Uses the authenticated scoped link and retains the original bounded queue age.
esp_err_t runtime_send_update(model::OctopusId,Kind,std::string payload,uint32_t ttl_ms=5000);
struct RuntimePeerStatus {
    model::OctopusId id;model::BootId boot_id;model::ExecutionLeaseId control_lease,realtime_lease;
    uint64_t config_revision=0,control_usable_until_ms=0,realtime_usable_until_ms=0;
    uint32_t negotiated_capabilities=0;
    uint8_t supported_tentacles=1,configured_tentacles_mask=1;
    std::string firmware_identity,hostname;
    bool management_ready=false,ready=false,enrolled=false,provisional=false,hardware_profile=false;
};
std::vector<RuntimePeerStatus> runtime_peers();
// Bounded public metadata retained offline/after removal. Never an allowlist.
std::vector<std::pair<model::OctopusId,std::string>> runtime_known_hostnames();
// Delivery and live authorization for original Amity actions.
esp_err_t runtime_send_owned(const model::OwnedIntent&);
esp_err_t runtime_send_owned_result(model::OctopusId,model::LinkId,std::string payload,uint32_t ttl_ms=1000);
void runtime_fence_owned_link(model::OctopusId,model::LinkId);
uint64_t runtime_owned_deadline(const model::OwnedAuthorization&);
bool runtime_owned_authorized(const model::OwnedAuthorization&);
std::optional<model::LinkId> runtime_link_id(model::OctopusId,model::BootId);
// Explicit physical USB-assisted bootstrap provisioning. Input must be generated
// by the provisioning tool. No password/verifier is generated automatically.
esp_err_t runtime_crypto_self_test();
esp_err_t runtime_rotate_bootstrap(model::OctopusId confirmed,const std::string &salt_hex,const std::string &verifier_hex);
esp_err_t runtime_install_bootstrap(const std::string &salt_hex,const std::string &verifier_hex);
} // namespace amity::consortium
