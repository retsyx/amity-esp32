/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// FreeRTOS on a scripted clock. As on the target, FreeRTOS.h brings in the
// task, queue and semaphore APIs and the ESP-IDF additions.
#include "freertos/portmacro.h"
#include "freertos/idf_additions.h"
