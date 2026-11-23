/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for components/config_store/json_memory.cpp.
#include "config_store.hpp"
#include "seams.hpp"

void config_store::initialize_json_memory() { AMITY_SEAM(config_store::initialize_json_memory); }
