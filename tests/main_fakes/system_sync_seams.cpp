/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for components/system_sync/system_sync.cpp.
#include "amity/system_sync.hpp"
#include "seams.hpp"

esp_err_t amity::start_configuration_synchronization(SystemControl &control) {
    return AMITY_SEAM(amity::start_configuration_synchronization, control);
}
esp_err_t amity::activate_distributed_configuration(config_store::Configuration candidate,
                                                    config_store::RevisionPair expected, system::MessageId request,
                                                    std::function<esp_err_t()> before_commit) {
    return AMITY_SEAM(amity::activate_distributed_configuration, std::move(candidate), expected, request,
                      std::move(before_commit));
}
amity::ConfigurationActivationStatus amity::configuration_activation_status(system::MessageId request) {
    return AMITY_SEAM(amity::configuration_activation_status, request);
}
std::vector<amity::system::CecObservation> amity::discovered_equipment() {
    return AMITY_SEAM(amity::discovered_equipment);
}
esp_err_t amity::request_equipment_scan() { return AMITY_SEAM(amity::request_equipment_scan); }
void amity::set_owner_maintenance(system::OctopusId owner, bool maintenance) {
    AMITY_SEAM(amity::set_owner_maintenance, owner, maintenance);
}
void amity::forget_owner(system::OctopusId owner) { AMITY_SEAM(amity::forget_owner, owner); }
