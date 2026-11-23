/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The Amity build's values that main/main.cpp reads, with the default board
// profile. The two settings that app_main tests with #if,
// CONFIG_SPIRAM_USE_MALLOC and CONFIG_AMITY_PRODUCTION_UPDATES, come from each
// test build's definitions instead: main_test builds once with neither and once
// with both.
#define CONFIG_LWIP_IPV6_NUM_ADDRESSES 6
