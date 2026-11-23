/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for components/system_services/adapter_settings.cpp.
#include "adapter_settings.hpp"
#include "seams.hpp"

void system_services::adapter_settings::install(amity::SystemControl &control) {
    AMITY_SEAM(system_services::adapter_settings::install, control);
}
std::string system_services::adapter_settings::snapshot() { return AMITY_SEAM(system_services::adapter_settings::snapshot); }
amity::consortium::Error system_services::adapter_settings::apply(amity::system::MessageId request_id,
                                                                  const amity::consortium::DeviceSettingsWrite &write,
                                                                  uint64_t receiver_deadline_ms, std::string &snapshot) {
    return AMITY_SEAM(system_services::adapter_settings::apply, request_id, write, receiver_deadline_ms, snapshot);
}
