/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// XIAO ESP32-S3: D0 = GPIO1, D1 = GPIO2.
#define AMITY_CEC_FRONT_GPIO 1
#define AMITY_CEC_BACK_GPIO 2
// Amity board2: active-low SW1 on XIAO D3 (GPIO4, module pad 4).
// Leave unconnected on buttonless devices.
#define AMITY_FW_RESET_GPIO 4
