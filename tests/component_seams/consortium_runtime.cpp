/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for the consortium runtime's public API, defined in
// components/consortium/runtime.cpp. Never link them with runtime.cpp.
#include "amity/consortium/runtime.hpp"
#include "seams.hpp"

esp_err_t amity::consortium::runtime_read_startup(model::OctopusId local, RuntimeStartup &startup) {
    return AMITY_SEAM(amity::consortium::runtime_read_startup, local, startup);
}
esp_err_t amity::consortium::runtime_set_hardware_profile(uint8_t supported_tentacles, uint8_t configured_tentacles_mask) {
    return AMITY_SEAM(amity::consortium::runtime_set_hardware_profile, supported_tentacles, configured_tentacles_mask);
}
esp_err_t amity::consortium::runtime_start(model::ConsortiumId consortium_id, model::OctopusId octopus, model::BootId boot,
                                    uint64_t config_revision) {
    return AMITY_SEAM(amity::consortium::runtime_start, consortium_id, octopus, boot, config_revision);
}
auto amity::consortium::runtime_status() -> RuntimeStatus { return AMITY_SEAM(amity::consortium::runtime_status); }
void amity::consortium::runtime_configuration_changed(uint64_t config_revision) {
    AMITY_SEAM(amity::consortium::runtime_configuration_changed, config_revision);
}
void amity::consortium::runtime_hostname_changed() { AMITY_SEAM(amity::consortium::runtime_hostname_changed); }
esp_err_t amity::consortium::runtime_claim_original_controller() { return AMITY_SEAM(amity::consortium::runtime_claim_original_controller); }
bool amity::consortium::runtime_original_setup_eligible() { return AMITY_SEAM(amity::consortium::runtime_original_setup_eligible); }
esp_err_t amity::consortium::runtime_open_enrollment(uint64_t radio_generation) {
    return AMITY_SEAM(amity::consortium::runtime_open_enrollment, radio_generation);
}
void amity::consortium::runtime_close_enrollment() { AMITY_SEAM(amity::consortium::runtime_close_enrollment); }
bool amity::consortium::runtime_cancel_radio_workflows() { return AMITY_SEAM(amity::consortium::runtime_cancel_radio_workflows); }
esp_err_t amity::consortium::runtime_register_gatt() { return AMITY_SEAM(amity::consortium::runtime_register_gatt); }
auto amity::consortium::runtime_admission_status() -> RuntimeAdmissionStatus {
    return AMITY_SEAM(amity::consortium::runtime_admission_status);
}
esp_err_t amity::consortium::runtime_scan_admission(model::MessageId scan_id) {
    return AMITY_SEAM(amity::consortium::runtime_scan_admission, scan_id);
}
esp_err_t amity::consortium::runtime_cancel_admission(model::MessageId scan_id) {
    return AMITY_SEAM(amity::consortium::runtime_cancel_admission, scan_id);
}
esp_err_t amity::consortium::runtime_add_octopus(model::MessageId scan_id, const std::string &candidate_token,
                                          const std::string &hostname) {
    return AMITY_SEAM(amity::consortium::runtime_add_octopus, scan_id, candidate_token, hostname);
}
esp_err_t amity::consortium::runtime_remove_octopus(model::OctopusId octopus, model::MessageId request_id,
                                             uint64_t expected_membership_revision,
                                             std::optional<model::BootId> expected_target_boot) {
    return AMITY_SEAM(amity::consortium::runtime_remove_octopus, octopus, request_id, expected_membership_revision,
                      expected_target_boot);
}
std::string amity::consortium::runtime_removal_operation(model::OctopusId octopus, model::MessageId request_id) {
    return AMITY_SEAM(amity::consortium::runtime_removal_operation, octopus, request_id);
}
esp_err_t amity::consortium::runtime_forget_removed_octopus(model::OctopusId octopus) {
    return AMITY_SEAM(amity::consortium::runtime_forget_removed_octopus, octopus);
}
void amity::consortium::runtime_radio_hooks(RuntimeRadioHooks hooks) { AMITY_SEAM(amity::consortium::runtime_radio_hooks, std::move(hooks)); }
void amity::consortium::runtime_network_hooks(RuntimeNetworkHooks hooks) {
    AMITY_SEAM(amity::consortium::runtime_network_hooks, std::move(hooks));
}
void amity::consortium::runtime_control_hooks(RuntimeControlHooks hooks) {
    AMITY_SEAM(amity::consortium::runtime_control_hooks, std::move(hooks));
}
void amity::consortium::runtime_lifecycle_hooks(RuntimeLifecycleHooks hooks) {
    AMITY_SEAM(amity::consortium::runtime_lifecycle_hooks, std::move(hooks));
}
void amity::consortium::runtime_settings_hooks(RuntimeSettingsHooks hooks) {
    AMITY_SEAM(amity::consortium::runtime_settings_hooks, std::move(hooks));
}
void amity::consortium::runtime_log_hooks(RuntimeLogHooks hooks) { AMITY_SEAM(amity::consortium::runtime_log_hooks, std::move(hooks)); }
esp_err_t amity::consortium::runtime_pull_logs(model::OctopusId owner, const std::function<bool(std::string_view)> &consume) {
    return AMITY_SEAM(amity::consortium::runtime_pull_logs, owner, consume);
}
std::string amity::consortium::runtime_device_settings(model::OctopusId owner) {
    return AMITY_SEAM(amity::consortium::runtime_device_settings, owner);
}
esp_err_t amity::consortium::runtime_write_device_settings(model::OctopusId owner, model::MessageId request_id,
                                                    const DeviceSettingsWrite &write, uint64_t admitted_until_ms) {
    return AMITY_SEAM(amity::consortium::runtime_write_device_settings, owner, request_id, write, admitted_until_ms);
}
std::string amity::consortium::runtime_device_settings_operation(model::OctopusId owner, model::MessageId request_id) {
    return AMITY_SEAM(amity::consortium::runtime_device_settings_operation, owner, request_id);
}
bool amity::consortium::runtime_device_settings_authorized(model::MessageId request_id) {
    return AMITY_SEAM(amity::consortium::runtime_device_settings_authorized, request_id);
}
void amity::consortium::runtime_update_hooks(RuntimeUpdateHooks hooks) { AMITY_SEAM(amity::consortium::runtime_update_hooks, std::move(hooks)); }
esp_err_t amity::consortium::runtime_send_update(model::OctopusId owner, Kind kind, std::string payload, uint32_t ttl_ms) {
    return AMITY_SEAM(amity::consortium::runtime_send_update, owner, kind, std::move(payload), ttl_ms);
}
esp_err_t amity::consortium::runtime_stage_update(model::OctopusId owner, std::string payload, uint32_t ttl_ms) {
    return AMITY_SEAM(amity::consortium::runtime_stage_update, owner, std::move(payload), ttl_ms);
}
auto amity::consortium::runtime_peers() -> std::vector<RuntimePeerStatus> { return AMITY_SEAM(amity::consortium::runtime_peers); }
auto amity::consortium::runtime_known_hostnames() -> std::vector<std::pair<model::OctopusId, std::string>> {
    return AMITY_SEAM(amity::consortium::runtime_known_hostnames);
}
esp_err_t amity::consortium::runtime_send_owned(const model::OwnedIntent &intent) {
    return AMITY_SEAM(amity::consortium::runtime_send_owned, intent);
}
esp_err_t amity::consortium::runtime_send_owned_result(model::OctopusId owner, const model::OwnedAdapterSnapshot &snapshot) {
    return AMITY_SEAM(amity::consortium::runtime_send_owned_result, owner, snapshot);
}
void amity::consortium::runtime_fence_owned_link(model::OctopusId owner, model::LinkId link) {
    AMITY_SEAM(amity::consortium::runtime_fence_owned_link, owner, link);
}
bool amity::consortium::runtime_owned_authorized(const model::OwnedAuthorization &authorization) {
    return AMITY_SEAM(amity::consortium::runtime_owned_authorized, authorization);
}
