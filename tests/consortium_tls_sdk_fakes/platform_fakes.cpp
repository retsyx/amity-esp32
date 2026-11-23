/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "platform_fakes.hpp"
#include "core_fakes.hpp"
#include "esp_random.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "scenario_main.hpp"
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <utility>

namespace {

// ESP_ERR_NVS_INVALID_LENGTH, for a read into a buffer smaller than the blob.
constexpr esp_err_t nvs_invalid_length = 0x110c;

struct Handle {
    std::string space;
    bool writable;
    std::map<std::string, std::string> pending;
};

struct State {
    std::mutex mutex;
    std::map<std::string, std::string> blobs;
    std::vector<std::string> writes;
    std::set<std::string> namespaces;
    std::map<nvs_handle_t, Handle> handles;
    nvs_handle_t next_handle = 1;
    std::map<std::pair<amity_tls_fake::Step, std::string>, esp_err_t> faults;
    std::function<void(const std::string &)> size_hook;
    std::deque<std::string> random;
    std::vector<amity_tls_fake::Wait> waits;
    bool real_time = false;
    std::chrono::steady_clock::time_point real_origin;
    int64_t scripted_origin = 0;
};
State &state() {
    static State instance;
    return instance;
}

// The scripted error for this step, consumed, or ESP_OK.
esp_err_t fault(amity_tls_fake::Step step, const std::string &name) {
    auto &faults = state().faults;
    const auto found = faults.find({step, name});
    if (found == faults.end()) return ESP_OK;
    const auto error = found->second;
    faults.erase(found);
    return error;
}

bool holds(const State &current, const std::string &space) {
    if (current.namespaces.count(space)) return true;
    const auto prefix = space + "/";
    const auto after = current.blobs.lower_bound(prefix);
    return after != current.blobs.end() && after->first.compare(0, prefix.size(), prefix) == 0;
}

Handle &handle(State &current, nvs_handle_t value, const char *function) {
    const auto found = current.handles.find(value);
    if (found == current.handles.end()) {
        const std::string message = std::string(function) + " was called with a handle that is not open";
        amity_test::fail(__FILE__, __LINE__, message.c_str());
    }
    return found->second;
}

int64_t microseconds(const timeval &value) { return int64_t(value.tv_sec) * 1000000 + value.tv_usec; }

}  // namespace

namespace amity_tls_fake {

std::map<std::string, std::string> &blobs() { return state().blobs; }
std::vector<std::string> &writes() { return state().writes; }
void erase_partition() {
    std::lock_guard<std::mutex> lock(state().mutex);
    state().blobs.clear();
    state().namespaces.clear();
}
unsigned open_handles() {
    std::lock_guard<std::mutex> lock(state().mutex);
    return unsigned(state().handles.size());
}
void fail(Step step, const std::string &name, esp_err_t error) {
    std::lock_guard<std::mutex> lock(state().mutex);
    state().faults[{step, name}] = error;
}
void on_size(std::function<void(const std::string &)> hook) {
    std::lock_guard<std::mutex> lock(state().mutex);
    state().size_hook = std::move(hook);
}
void script_random(std::string bytes) {
    std::lock_guard<std::mutex> lock(state().mutex);
    state().random.push_back(std::move(bytes));
}
bool scripts_used() {
    std::lock_guard<std::mutex> lock(state().mutex);
    return state().faults.empty() && state().random.empty();
}
std::vector<Wait> waits() {
    std::lock_guard<std::mutex> lock(state().mutex);
    return state().waits;
}
void follow_real_time() {
    std::lock_guard<std::mutex> lock(state().mutex);
    state().real_time = true;
    state().real_origin = std::chrono::steady_clock::now();
    state().scripted_origin = amity_fake::now_us();
}
void catch_up() {
    std::lock_guard<std::mutex> lock(state().mutex);
    if (!state().real_time) amity_test::fail(__FILE__, __LINE__, "catch_up() needs follow_real_time()");
    const auto real = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                            state().real_origin).count();
    const auto scripted = amity_fake::now_us() - state().scripted_origin;
    if (real > scripted) amity_fake::advance_us(real - scripted);
}

}  // namespace amity_tls_fake

using amity_tls_fake::Step;

esp_err_t nvs_open_from_partition(const char *partition, const char *space, nvs_open_mode_t mode, nvs_handle_t *out) {
    auto &current = state();
    std::lock_guard<std::mutex> lock(current.mutex);
    if (std::strcmp(partition, "amity_cfg") != 0) {
        const std::string message = std::string("NVS partition ") + partition + " does not exist";
        amity_test::fail(__FILE__, __LINE__, message.c_str());
    }
    if (const auto error = fault(Step::open, space)) return error;
    if (mode == NVS_READONLY && !holds(current, space)) return ESP_ERR_NVS_NOT_FOUND;
    if (mode == NVS_READWRITE) current.namespaces.insert(space);
    *out = current.next_handle++;
    current.handles[*out] = {space, mode == NVS_READWRITE, {}};
    return ESP_OK;
}

void nvs_close(nvs_handle_t value) {
    auto &current = state();
    std::lock_guard<std::mutex> lock(current.mutex);
    handle(current, value, "nvs_close");
    current.handles.erase(value);
}

esp_err_t nvs_get_blob(nvs_handle_t value, const char *key, void *out, size_t *length) {
    auto &current = state();
    std::function<void(const std::string &)> hook;
    std::string name;
    {
        std::lock_guard<std::mutex> lock(current.mutex);
        name = handle(current, value, "nvs_get_blob").space + "/" + key;
        if (const auto error = fault(out ? Step::read : Step::size, name)) return error;
        const auto found = current.blobs.find(name);
        if (found == current.blobs.end()) return ESP_ERR_NVS_NOT_FOUND;
        if (out) {
            if (*length < found->second.size()) return nvs_invalid_length;
            std::memcpy(out, found->second.data(), found->second.size());
            *length = found->second.size();
            return ESP_OK;
        }
        *length = found->second.size();
        hook = current.size_hook;
    }
    if (hook) hook(name);
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t value, const char *key, const void *bytes, size_t length) {
    auto &current = state();
    std::lock_guard<std::mutex> lock(current.mutex);
    auto &open = handle(current, value, "nvs_set_blob");
    if (!open.writable) amity_test::fail(__FILE__, __LINE__, "nvs_set_blob on a read-only handle");
    const auto name = open.space + "/" + key;
    current.writes.push_back(name);
    if (const auto error = fault(Step::write, name)) return error;
    open.pending[key].assign(static_cast<const char *>(bytes), length);
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t value) {
    auto &current = state();
    std::lock_guard<std::mutex> lock(current.mutex);
    auto &open = handle(current, value, "nvs_commit");
    if (const auto error = fault(Step::commit, open.space)) return error;
    for (auto &[key, bytes] : open.pending) current.blobs[open.space + "/" + key] = std::move(bytes);
    open.pending.clear();
    return ESP_OK;
}

void esp_fill_random(void *bytes, size_t size) {
    std::string scripted;
    bool have = false;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        if (!state().random.empty()) {
            scripted = std::move(state().random.front());
            state().random.pop_front();
            have = true;
        }
    }
    if (have) {
        if (scripted.size() != size)
            amity_test::fail(__FILE__, __LINE__, "scripted random bytes have the wrong length");
        std::memcpy(bytes, scripted.data(), size);
        return;
    }
    if (psa_generate_random(static_cast<uint8_t *>(bytes), size) != PSA_SUCCESS)
        amity_test::fail(__FILE__, __LINE__, "psa_generate_random failed; call psa_crypto_init first");
}

int lwip_select(int maxfdp1, fd_set *readset, fd_set *writeset, fd_set *exceptset, struct timeval *timeout) {
    auto &current = state();
    bool real_time;
    {
        std::lock_guard<std::mutex> lock(current.mutex);
        real_time = current.real_time;
        if (!real_time && current.waits.size() >= 100000)
            amity_test::fail(__FILE__, __LINE__, "a wait loop never ends on the scripted clock");
    }
    if (!real_time && !timeout) amity_test::fail(__FILE__, __LINE__, "an endless select on the scripted clock");
    const amity_tls_fake::Wait wait{readset != nullptr, writeset != nullptr, timeout ? microseconds(*timeout) : -1,
                                    false};
    int result;
    if (real_time) {
        result = (::select)(maxfdp1, readset, writeset, exceptset, timeout);
        amity_tls_fake::catch_up();
    } else {
        timeval now{0, 0};
        result = (::select)(maxfdp1, readset, writeset, exceptset, &now);
        if (result == 0) amity_fake::advance_us(microseconds(*timeout));
    }
    std::lock_guard<std::mutex> lock(current.mutex);
    current.waits.push_back(wait);
    current.waits.back().ready = result > 0;
    return result;
}
