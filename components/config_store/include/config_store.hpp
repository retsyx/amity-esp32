/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include "esp_err.h"
#include "nvs.h"
#include <string>
#include <functional>
namespace config_store {
// Dedicated partition, independent of Matter's network/fabric storage. Never erase on error.
esp_err_t init();
// Bounded single-flight flash broker; callbacks execute on its internal stack.
bool with_internal_flash_stack(std::function<void()>);
// Capacity preflight for an entire immutable blob/candidate plus any dependent
// decision records. Uses available_entries, preserving IDF's GC reserve page.
esp_err_t check_blob_storage(size_t bytes, bool final_decision = false);
struct StorageBudget { size_t used_entries=0, available_entries=0, total_entries=0; size_t configuration_limit=16384; };
esp_err_t storage_budget(StorageBudget&);
esp_err_t read(const char *key, std::string &value);
esp_err_t write(const char *key, const std::string &value);
}
