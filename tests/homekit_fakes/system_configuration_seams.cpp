/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Strict seams for the saved-configuration readers homekit.cpp calls, from
// components/config_store/system_configuration.cpp. That file needs cJSON,
// so tests that build it are in the sdk tier.
#include "system_configuration.hpp"
#include "seams.hpp"

const config_store::ConfiguredActivity *config_store::activity_for_slot(const Configuration &configuration,
                                                                         unsigned slot) {
    return AMITY_SEAM(config_store::activity_for_slot, configuration, slot);
}
std::vector<config_store::ExternalInput> config_store::external_inputs(const Configuration &configuration) {
    return AMITY_SEAM(config_store::external_inputs, configuration);
}
std::vector<config_store::model::ActivityId> config_store::ordered_activities(const Configuration &configuration) {
    return AMITY_SEAM(config_store::ordered_activities, configuration);
}
