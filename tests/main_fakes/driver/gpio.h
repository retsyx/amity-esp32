/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The ESP32-S3's GPIO validity checks (checked by sdk_parity/main.def).
#define SOC_GPIO_PIN_COUNT 49
#define SOC_GPIO_VALID_GPIO_MASK (0x1FFFFFFFFFFFFULL & ~(0ULL | (1ULL << 22) | (1ULL << 23) | (1ULL << 24) | (1ULL << 25)))
#define SOC_GPIO_VALID_OUTPUT_GPIO_MASK (SOC_GPIO_VALID_GPIO_MASK)
#define GPIO_IS_VALID_GPIO(gpio_num) \
    ((gpio_num >= 0) && (gpio_num < SOC_GPIO_PIN_COUNT) && (((1ULL << (gpio_num)) & SOC_GPIO_VALID_GPIO_MASK) != 0))
#define GPIO_IS_VALID_OUTPUT_GPIO(gpio_num) \
    ((gpio_num >= 0) && (gpio_num < SOC_GPIO_PIN_COUNT) && (((1ULL << (gpio_num)) & SOC_GPIO_VALID_OUTPUT_GPIO_MASK) != 0))
