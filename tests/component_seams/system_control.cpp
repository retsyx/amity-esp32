/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for the out-of-line SystemControl members of
// components/amity_hub/system_control.cpp. The inline members call the
// HubService seams. Its private members are used only inside that file.
#include "amity/system_control.hpp"
#include "seams.hpp"

using amity::SystemControl;

esp_err_t SystemControl::transition_coordinator_authority(bool enabled) {
    return AMITY_SEAM(amity::SystemControl::transition_coordinator_authority, *this, enabled);
}
bool SystemControl::accepts_local_input() const { return AMITY_SEAM(amity::SystemControl::accepts_local_input, *this); }
esp_err_t SystemControl::request_activity(int index) {
    return AMITY_SEAM_AS(esp_err_t (SystemControl::*)(int), amity::SystemControl::request_activity, *this, index);
}
bool SystemControl::press_key(uint8_t key, bool repeat) {
    return AMITY_SEAM(amity::SystemControl::press_key, *this, key, repeat);
}
esp_err_t SystemControl::request_remote_activity(unsigned index) {
    return AMITY_SEAM_AS(esp_err_t (SystemControl::*)(unsigned), amity::SystemControl::request_remote_activity, *this,
                         index);
}
esp_err_t SystemControl::request_remote_activity(unsigned index, uint32_t generation) {
    return AMITY_SEAM_AS(esp_err_t (SystemControl::*)(unsigned, uint32_t), amity::SystemControl::request_remote_activity,
                         *this, index, generation);
}
esp_err_t SystemControl::repair_remote_activity(uint32_t generation) {
    return AMITY_SEAM(amity::SystemControl::repair_remote_activity, *this, generation);
}
esp_err_t SystemControl::stop_remote_zone(uint32_t generation) {
    return AMITY_SEAM_AS(esp_err_t (SystemControl::*)(uint32_t), amity::SystemControl::stop_remote_zone, *this,
                         generation);
}
esp_err_t SystemControl::configure(config_store::ConfigurationSnapshot snapshot, std::function<esp_err_t()> commit) {
    return AMITY_SEAM(amity::SystemControl::configure, *this, std::move(snapshot), std::move(commit));
}
esp_err_t SystemControl::configure_passive_metadata(config_store::ConfigurationSnapshot snapshot) {
    return AMITY_SEAM(amity::SystemControl::configure_passive_metadata, *this, std::move(snapshot));
}
esp_err_t SystemControl::request_activity(system::ActivityId activity, system::MessageId request,
                                          std::optional<uint64_t> expected) {
    return AMITY_SEAM_AS(
        esp_err_t (SystemControl::*)(amity::system::ActivityId, amity::system::MessageId, std::optional<uint64_t>),
        amity::SystemControl::request_activity, *this, activity, request, expected);
}
auto SystemControl::execution_snapshot() const -> system::ExecutionSnapshot {
    return AMITY_SEAM(amity::SystemControl::execution_snapshot, *this);
}
auto SystemControl::applied_configuration() const -> AppliedConfiguration {
    return AMITY_SEAM(amity::SystemControl::applied_configuration, *this);
}
auto SystemControl::native_observations() const -> std::vector<system::CecObservation> {
    return AMITY_SEAM(amity::SystemControl::native_observations, *this);
}
auto SystemControl::external_activity(unsigned index) const -> std::optional<config_store::ConfiguredActivity> {
    return AMITY_SEAM(amity::SystemControl::external_activity, *this, index);
}
std::optional<unsigned> SystemControl::external_activity_index(const Snapshot &snapshot) const {
    return AMITY_SEAM_AS(std::optional<unsigned> (SystemControl::*)(const amity::Snapshot &) const,
                         amity::SystemControl::external_activity_index, *this, snapshot);
}
std::optional<unsigned> SystemControl::external_activity_index(const PlaybackState &state) const {
    return AMITY_SEAM_AS(std::optional<unsigned> (SystemControl::*)(const amity::PlaybackState &) const,
                         amity::SystemControl::external_activity_index, *this, state);
}
