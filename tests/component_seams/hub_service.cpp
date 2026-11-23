/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for the public HubService members of components/amity_hub/hub_service.cpp.
// The constructor and destructor only record their call.
#include "amity/hub_service.hpp"
#include "seams.hpp"

using amity::HubService;

struct HubService::Impl {};

HubService::HubService() { amity_seam::record("amity::HubService::HubService"); }
HubService::~HubService() { amity_seam::record("amity::HubService::~HubService"); }
esp_err_t HubService::start(const cec::BoardProfile &profile, const std::array<uint16_t, amity::max_tentacles> &addresses) {
    return AMITY_SEAM(amity::HubService::start, *this, profile, addresses);
}
auto HubService::adapter_runtime_snapshot() const -> std::vector<AdapterRuntimeState> {
    return AMITY_SEAM(amity::HubService::adapter_runtime_snapshot, *this);
}
esp_err_t HubService::commit_adapter_settings(system::BootId boot, std::vector<AdapterAddressChange> changes,
                                              uint64_t deadline, std::function<esp_err_t()> persist) {
    return AMITY_SEAM(amity::HubService::commit_adapter_settings, *this, boot, std::move(changes), deadline,
                      std::move(persist));
}
esp_err_t HubService::request_activity(int index, uint32_t *accepted_selection, std::optional<system::ActivityId> expected) {
    return AMITY_SEAM_AS(esp_err_t (HubService::*)(int, uint32_t *, std::optional<amity::system::ActivityId>),
                         amity::HubService::request_activity, *this, index, accepted_selection, expected);
}
esp_err_t HubService::repair_activity() { return AMITY_SEAM(amity::HubService::repair_activity, *this); }
uint32_t HubService::activity_selection() const { return AMITY_SEAM(amity::HubService::activity_selection, *this); }
int HubService::activity_target() const { return AMITY_SEAM(amity::HubService::activity_target, *this); }
bool HubService::press_key(uint8_t key, bool repeat) { return AMITY_SEAM(amity::HubService::press_key, *this, key, repeat); }
bool HubService::press_key_for_activity(uint8_t key, uint32_t generation, bool repeat) {
    return AMITY_SEAM(amity::HubService::press_key_for_activity, *this, key, generation, repeat);
}
uint32_t HubService::input_epoch() const { return AMITY_SEAM(amity::HubService::input_epoch, *this); }
uint32_t HubService::activity_generation() const { return AMITY_SEAM(amity::HubService::activity_generation, *this); }
bool HubService::release_key(uint8_t key) { return AMITY_SEAM(amity::HubService::release_key, *this, key); }
bool HubService::standby() { return AMITY_SEAM(amity::HubService::standby, *this); }
bool HubService::discover() { return AMITY_SEAM(amity::HubService::discover, *this); }
auto HubService::activity_state() const -> ActivityState { return AMITY_SEAM(amity::HubService::activity_state, *this); }
auto HubService::snapshot() const -> Snapshot { return AMITY_SEAM(amity::HubService::snapshot, *this); }
void HubService::on_change(std::function<void(const Snapshot &)> callback) {
    AMITY_SEAM(amity::HubService::on_change, *this, std::move(callback));
}
auto HubService::playback_state() const -> PlaybackState { return AMITY_SEAM(amity::HubService::playback_state, *this); }
void HubService::on_configuration_applied(std::function<void()> callback) {
    AMITY_SEAM(amity::HubService::on_configuration_applied, *this, std::move(callback));
}
bool HubService::activity_setup_in_progress() const {
    return AMITY_SEAM(amity::HubService::activity_setup_in_progress, *this);
}
void HubService::set_system_identity(system::ConsortiumId consortium, system::OctopusId octopus) {
    AMITY_SEAM(amity::HubService::set_system_identity, *this, consortium, octopus);
}
void HubService::set_boot_identity(system::BootId boot) { AMITY_SEAM(amity::HubService::set_boot_identity, *this, boot); }
void HubService::set_coordinator_authority(bool enabled) {
    AMITY_SEAM(amity::HubService::set_coordinator_authority, *this, enabled);
}
esp_err_t HubService::transition_coordinator_authority(bool enabled) {
    return AMITY_SEAM(amity::HubService::transition_coordinator_authority, *this, enabled);
}
esp_err_t HubService::retire_membership() { return AMITY_SEAM(amity::HubService::retire_membership, *this); }
esp_err_t HubService::begin_maintenance() { return AMITY_SEAM(amity::HubService::begin_maintenance, *this); }
void HubService::end_maintenance() { AMITY_SEAM(amity::HubService::end_maintenance, *this); }
bool HubService::maintenance_ready() const { return AMITY_SEAM(amity::HubService::maintenance_ready, *this); }
esp_err_t HubService::configure_scoped(std::vector<Activity> activities, std::vector<system::ScopedSelector> selectors,
                                       OwnedSender sender, system::OctopusId controller, uint64_t config_revision,
                                       std::function<esp_err_t()> commit, uint64_t presentation_revision,
                                       std::vector<system::OctopusId> owners,
                                       std::function<bool(const Snapshot &)> can_commit) {
    return AMITY_SEAM(amity::HubService::configure_scoped, *this, std::move(activities), std::move(selectors),
                      std::move(sender), controller, config_revision, std::move(commit), presentation_revision,
                      std::move(owners), std::move(can_commit));
}
auto HubService::submit_owned(const system::OwnedIntent &intent, const system::OwnedAuthorization &authorization,
                              uint32_t timeout_ms) -> system::AdmissionResult {
    return AMITY_SEAM(amity::HubService::submit_owned, *this, intent, authorization, timeout_ms);
}
auto HubService::submit_key_edges(const std::array<system::KeyEdge, 2> &edges, uint8_t count, uint64_t activity_generation,
                                  system::LinkId link, uint64_t deadline_ms, uint32_t timeout_ms) -> system::AdmissionResult {
    return AMITY_SEAM(amity::HubService::submit_key_edges, *this, edges, count, activity_generation, link, deadline_ms,
                      timeout_ms);
}
esp_err_t HubService::bind_owned_controller(system::OctopusId controller, uint64_t config_revision,
                                            system::BootId controller_boot) {
    return AMITY_SEAM(amity::HubService::bind_owned_controller, *this, controller, config_revision, controller_boot);
}
void HubService::set_owned_authorizer(std::function<bool(const system::OwnedAuthorization &)> check) {
    AMITY_SEAM(amity::HubService::set_owned_authorizer, *this, std::move(check));
}
void HubService::set_owned_observer(std::function<void(const system::OwnedAdapterSnapshot &)> observer) {
    AMITY_SEAM(amity::HubService::set_owned_observer, *this, std::move(observer));
}
bool HubService::update_owned_snapshot(system::OwnedAdapterSnapshot report) {
    return AMITY_SEAM(amity::HubService::update_owned_snapshot, *this, std::move(report));
}
void HubService::forget_owned_snapshots(system::OctopusId owner) {
    AMITY_SEAM(amity::HubService::forget_owned_snapshots, *this, owner);
}
bool HubService::refresh_owned_context(system::OctopusId owner, uint64_t expected_revision,
                                       std::function<void(system::OwnedContextResult)> completed) {
    return AMITY_SEAM(amity::HubService::refresh_owned_context, *this, owner, expected_revision, std::move(completed));
}
auto HubService::owned_snapshots() const -> std::vector<system::OwnedAdapterSnapshot> {
    return AMITY_SEAM(amity::HubService::owned_snapshots, *this);
}
bool HubService::fence_owned(system::OctopusId controller, system::LinkId link) {
    return AMITY_SEAM(amity::HubService::fence_owned, *this, controller, link);
}
bool HubService::discover_owned(system::OctopusId owner, uint8_t adapter) {
    return AMITY_SEAM(amity::HubService::discover_owned, *this, owner, adapter);
}
uint64_t HubService::executing_configuration_revision() const {
    return AMITY_SEAM(amity::HubService::executing_configuration_revision, *this);
}
