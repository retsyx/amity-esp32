/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// Scenario controls for the platform under credentials.cpp and tls_link.cpp
// when they run against the host Mbed TLS: NVS, random fill and select.
//
// NVS keeps blobs in memory. Only the amity_cfg partition exists, and a
// read-only open of a namespace that holds nothing fails with
// ESP_ERR_NVS_NOT_FOUND. A blob set through a handle is stored when that
// handle commits; closing it first discards the blob.
//
// esp_fill_random returns PSA random bytes unless a scenario scripted them.
//
// select waits on the scenario's clock. By default it never blocks: when
// nothing is ready, the scripted clock advances by the whole timeout, as if
// the wait had run out. After follow_real_time() it blocks as the host's
// select does, and the scripted clock follows real time.
#include "esp_err.h"
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace amity_tls_fake {

// Committed blobs by "namespace/key". Scenarios may edit them directly.
std::map<std::string, std::string> &blobs();
// Every nvs_set_blob call, as "namespace/key", in call order.
std::vector<std::string> &writes();
// Removes every blob and namespace, like a freshly erased partition.
void erase_partition();
// Handles opened and not yet closed.
unsigned open_handles();

enum class Step { open, size, read, write, commit };
// The next step on name fails with error. name is "namespace/key", or the
// namespace alone for open and commit.
void fail(Step step, const std::string &name, esp_err_t error);
// Runs after nvs_get_blob reports a blob's size and before the caller reads it.
void on_size(std::function<void(const std::string &name)> hook);

// The next esp_fill_random call returns these bytes; it must ask for exactly as many.
void script_random(std::string bytes);
// Whether every scripted failure and random fill has been used.
bool scripts_used();

struct Wait {
    bool readable, writable;
    int64_t timeout_us;
    bool ready;
};
// Every select call, in call order.
std::vector<Wait> waits();
void follow_real_time();
// Advances the scripted clock to real time, after follow_real_time().
void catch_up();

}  // namespace amity_tls_fake
