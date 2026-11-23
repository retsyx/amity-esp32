/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for components/config_store/system_configuration.cpp, which needs
// cJSON and so builds only in the sdk tier. configuration_journal.cpp, which
// holds ConfigurationStore, has no seams: it builds on the host.
#include "system_configuration.hpp"
#include "seams.hpp"

namespace config_store {

bool initialize_configuration(model::ConsortiumId consortium, model::OctopusId controller, uint8_t supported_tentacles,
                              Configuration &configuration, std::string &error) {
    return AMITY_SEAM(config_store::initialize_configuration, consortium, controller, supported_tentacles, configuration,
                      error);
}
bool parse_configuration(const std::string &input, Configuration &out, std::string &error) {
    return AMITY_SEAM(config_store::parse_configuration, input, out, error);
}
std::string serialize_configuration(const Configuration &configuration, bool executable_policy_only) {
    return AMITY_SEAM(config_store::serialize_configuration, configuration, executable_policy_only);
}
std::string sha256_hex(std::string_view input) { return AMITY_SEAM(config_store::sha256_hex, input); }
std::string executable_policy_hash(const Configuration &configuration) {
    return AMITY_SEAM(config_store::executable_policy_hash, configuration);
}
std::string presentation_hash(const Configuration &configuration) {
    return AMITY_SEAM(config_store::presentation_hash, configuration);
}
const ConfiguredActivity *activity_for_slot(const Configuration &configuration, unsigned slot) {
    return AMITY_SEAM(config_store::activity_for_slot, configuration, slot);
}
std::vector<ExternalInput> external_inputs(const Configuration &configuration) {
    return AMITY_SEAM(config_store::external_inputs, configuration);
}
bool retains_activity(const Configuration &previous, const Configuration &candidate, model::ActivityId activity) {
    return AMITY_SEAM(config_store::retains_activity, previous, candidate, activity);
}
std::vector<model::ActivityId> ordered_activities(const Configuration &configuration) {
    return AMITY_SEAM(config_store::ordered_activities, configuration);
}
bool reconcile_external_slots(const Configuration &previous, Configuration &candidate, std::string &error) {
    return AMITY_SEAM(config_store::reconcile_external_slots, previous, candidate, error);
}
bool propose_octopus_removal(const Configuration &previous, model::OctopusId owner, Configuration &candidate,
                             std::vector<model::ActivityId> &affected, std::string &error) {
    return AMITY_SEAM(config_store::propose_octopus_removal, previous, owner, candidate, affected, error);
}
bool validate_configuration(const Configuration &configuration, std::string &error) {
    return AMITY_SEAM(config_store::validate_configuration, configuration, error);
}
bool reconcile_hardware_profile(Configuration &configuration, model::OctopusId owner, uint8_t capacity, bool &changed,
                                std::string &error) {
    return AMITY_SEAM(config_store::reconcile_hardware_profile, configuration, owner, capacity, changed, error);
}

}  // namespace config_store
