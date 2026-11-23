/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for components/device_settings/device_settings.cpp.
#include "device_settings.hpp"
#include "seams.hpp"

esp_err_t device_settings::init() { return AMITY_SEAM(device_settings::init); }
const amity::FactoryCredentials *device_settings::factory() { return AMITY_SEAM(device_settings::factory); }
const char *device_settings::hostname() { return AMITY_SEAM(device_settings::hostname); }
auto device_settings::hostname_settings() -> HostnameSettings { return AMITY_SEAM(device_settings::hostname_settings); }
esp_err_t device_settings::save_hostname(uint64_t expected_revision, std::string hostname) {
    return AMITY_SEAM(device_settings::save_hostname, expected_revision, std::move(hostname));
}
const char *device_settings::initial_admin_auth() { return AMITY_SEAM(device_settings::initial_admin_auth); }
uint16_t device_settings::front_physical_address() { return AMITY_SEAM(device_settings::front_physical_address); }
auto device_settings::adapter_defaults() -> AdapterDefaults { return AMITY_SEAM(device_settings::adapter_defaults); }
esp_err_t device_settings::save_adapter_defaults(uint64_t expected_revision, const std::vector<AdapterDefaultChange> &changes) {
    return AMITY_SEAM(device_settings::save_adapter_defaults, expected_revision, changes);
}
void device_settings::set_hostname_handler(HostnameHandler handler) {
    AMITY_SEAM(device_settings::set_hostname_handler, std::move(handler));
}
bool device_settings::normalize_hostname(std::string &value) { return AMITY_SEAM(device_settings::normalize_hostname, value); }
esp_err_t device_settings::apply_enrollment_hostname(std::string value) {
    return AMITY_SEAM(device_settings::apply_enrollment_hostname, std::move(value));
}
