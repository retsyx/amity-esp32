/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once

#include "esp_err.h"

namespace amity_matter {
// Call after ESP Matter starts, with the CHIP stack lock held.
esp_err_t start_apple_fabric_cleanup();
}
