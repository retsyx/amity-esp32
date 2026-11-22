// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/system/model.hpp"
#include "esp_err.h"
namespace amity { class SystemControl; }
namespace system_services {
// Authenticated explicit removal only, after the runtime's lifecycle retire
// hook has fenced the board. Returns once the factory reset marker is
// committed on the internal flash stack. The main is never eligible.
esp_err_t prepare_removed_device_reset(amity::SystemControl&,
    amity::system::OctopusId authenticated_coordinator);
// A durable reset is not canceled by a lost reply. Invoke after attempting its
// result transmission; never on queue admission or a failed prepare callback.
void removed_device_reset_response_attempted();
}
