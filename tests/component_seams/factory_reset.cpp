/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for components/device_settings/factory_reset.cpp.
#include "device_settings.hpp"
#include "seams.hpp"

esp_err_t device_settings::finish_factory_reset() { return AMITY_SEAM(device_settings::finish_factory_reset); }
esp_err_t device_settings::schedule_factory_reset() { return AMITY_SEAM(device_settings::schedule_factory_reset); }
bool device_settings::reset_pending() { return AMITY_SEAM(device_settings::reset_pending); }
