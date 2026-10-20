// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "firmware_update.hpp"
#include "amity/consortium/runtime.hpp"
namespace system_services::updates {
struct CohortHooks {
    std::function<void(amity::system::OctopusId,bool)> owner_maintenance;
    std::function<uint64_t()> applied_config_revision;
    std::function<std::string()> applied_policy_hash;
};
// Called before the operational synchronization service starts accepting peers.
void start_group_updates(amity::system::OctopusId,CohortHooks);
esp_err_t stage_cohort(std::string_view manifest,std::string_view signature_hex,
    std::vector<amity::system::OctopusId> targets,std::string& error);
esp_err_t cohort_chunk(amity::system::MessageId job,amity::system::OctopusId target,uint64_t offset,std::string_view bytes,std::string& error);
esp_err_t cohort_install(amity::system::MessageId job,amity::system::OctopusId target,std::string& error);
esp_err_t cancel_cohort(amity::system::MessageId job,std::string& error);
std::string cohort_status_json();
void poll_group_update();
} // namespace system_services::updates
