/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for components/config_store/system_identity.cpp, which only the target builds.
#include "system_identity.hpp"
#include "seams.hpp"

esp_err_t config_store::load_system_identity(amity::system::OctopusId &octopus, amity::system::ConsortiumId &consortium) {
    return AMITY_SEAM(config_store::load_system_identity, octopus, consortium);
}
