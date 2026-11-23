/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for components/config_store/system_configuration_nvs.cpp, which only
// the target builds: the flash broker and the NVS-backed configuration journal.
#include "config_store.hpp"
#include "system_configuration.hpp"
#include "seams.hpp"

bool config_store::with_internal_flash_stack(std::function<void()> function) {
    return AMITY_SEAM(config_store::with_internal_flash_stack, std::move(function));
}
esp_err_t config_store::load_system_configuration(ConfigurationSnapshot &out, std::string &error) {
    return AMITY_SEAM(config_store::load_system_configuration, out, error);
}
esp_err_t config_store::stage_system_configuration(Configuration candidate, RevisionPair expected, std::string &error) {
    return AMITY_SEAM(config_store::stage_system_configuration, std::move(candidate), expected, error);
}
esp_err_t config_store::activate_system_configuration(ConfigurationSnapshot &out, std::string &error) {
    return AMITY_SEAM(config_store::activate_system_configuration, out, error);
}
esp_err_t config_store::staged_system_configuration(ConfigurationSnapshot &out) {
    return AMITY_SEAM(config_store::staged_system_configuration, out);
}
esp_err_t config_store::cancel_staged_configuration() { return AMITY_SEAM(config_store::cancel_staged_configuration); }
