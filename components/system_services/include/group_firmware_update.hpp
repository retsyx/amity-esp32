// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "firmware_update.hpp"
#include "amity/consortium/runtime.hpp"
namespace system_services::updates {
struct CohortHooks {
    std::function<void(amity::system::OctopusId,bool)> owner_maintenance;
};
void start_group_updates(amity::system::OctopusId,CohortHooks);
// One authenticated browser upload stages the primary. Distribution and
// activation proceed independently of the browser on the update worker.
esp_err_t begin_system_upload(const ReleaseManifest&,std::string& error);
void system_upload_progress(uint64_t bytes);
esp_err_t finish_system_upload(std::string& error);
void fail_system_upload(const std::string& error);
std::string cohort_status_json();
// Call only from the dedicated update task, never the owner/CEC task. Runs one
// step and returns how long the task may wait for the next one; a finished
// browser upload ends that wait early.
uint32_t poll_group_update();
void wait_group_update(uint32_t milliseconds);
} // namespace system_services::updates
