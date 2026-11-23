/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// Shared setup for the tests that build components/amity_homekit/homekit.cpp.
// A test includes homekit.cpp and then this header, which defines the seam of
// setup.c's amity_homekit_generate_setup against homekit.cpp's own
// declaration. install_defaults() gives every seam homekit.cpp calls a
// behaviour; start() starts HomeKit with its worker task running while
// amity_homekit::start() waits for it, as the scheduler would run it.
#include "core_fakes.hpp"
#include "hap_fake.hpp"
#include "seams.hpp"
#include "esp_timer.h"
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <string>

extern "C" esp_err_t amity_homekit_generate_setup(const char *code, hap_setup_info_t *setup) {
    return AMITY_SEAM(amity_homekit_generate_setup, code, setup);
}

namespace homekit_harness {

namespace hap = amity_fake::hap;

constexpr const char *hostname = "amity-test";
constexpr uint8_t mac[6] = {0x24, 0x6f, 0x28, 0xa1, 0xb2, 0xc3};

struct State {
    // The depth of the HAP lock homekit.cpp holds.
    int hap_lock = 0;
    // Runs at the worker's loop passes, one per pass, after the pass's queue
    // wait; the worker leaves at the pass after the last.
    std::deque<std::function<void()>> passes;
    unsigned loop_passes = 0;
    bool network = true;
    // config_store entries; a missing entry reads as ESP_ERR_NVS_NOT_FOUND.
    std::map<std::string, std::string> saved;
    std::map<std::string, esp_err_t> read_errors, write_errors;
    std::vector<std::string> writes;
    // esp_random results, in order, before a counter takes over.
    std::deque<uint32_t> random;
    uint32_t counter = 0x5a5a;
    uint8_t fill = 0;
    ble_remote_status_t remote{};
    amity::PlaybackState playback;
};

inline State &state() {
    static State instance;
    return instance;
}

// A saved setup record: the four-character setup ID, then the SRP salt and
// verifier, as prepare_setup() writes it.
inline hap_setup_info_t setup_info(uint8_t seed) {
    hap_setup_info_t setup{};
    for (size_t i = 0; i < sizeof(setup.salt); ++i) setup.salt[i] = uint8_t(seed + i);
    for (size_t i = 0; i < sizeof(setup.verifier); ++i) setup.verifier[i] = uint8_t(seed ^ i);
    return setup;
}
inline std::string setup_record(const std::string &id = "SAVE", uint8_t seed = 7) {
    const auto setup = setup_info(seed);
    return id + std::string(reinterpret_cast<const char *>(&setup), sizeof(setup));
}

inline void install_defaults() {
    using amity_seam::install;
    install<&device_settings::hostname>([] { return hostname; });
    install<&hap_amity_init>([](const char *name) {
        CHECK(name && std::strcmp(name, hostname) == 0);
        return HAP_SUCCESS;
    });
    install<&hap_amity_lock>([] { ++state().hap_lock; });
    install<&hap_amity_unlock>([] {
        CHECK(state().hap_lock > 0);
        --state().hap_lock;
    });
    install<&hap_amity_set_pairing_deadline>([](int64_t) {});
    install<&hap_amity_refresh_mdns>([] { return HAP_SUCCESS; });
    install<&hap_amity_remove_pairings>([] { return HAP_SUCCESS; });
    // amity_hap.c takes the HAP lock itself and releases it before stopping
    // the HTTP server, which can wait for a handler that wants the lock.
    install<&hap_amity_stop>([] {
        CHECK(state().hap_lock == 0);
        return HAP_SUCCESS;
    });
    install<&esp_wifi_sta_get_ap_info>([](wifi_ap_record_t *record) {
        CHECK(record != nullptr);
        // The startup check runs under the HAP lock; each loop pass checks the
        // network outside it, right after the pass's queue wait.
        if (!state().hap_lock) {
            ++state().loop_passes;
            if (state().passes.empty()) throw amity_fake::LeaveTask{};
            const auto pass = std::move(state().passes.front());
            state().passes.pop_front();
            pass();
        }
        return state().network ? ESP_OK : ESP_ERR_WIFI_NOT_CONNECT;
    });
    install<&esp_read_mac>([](uint8_t *out, esp_mac_type_t type) {
        CHECK(type == ESP_MAC_WIFI_STA);
        std::memcpy(out, mac, sizeof(mac));
        return ESP_OK;
    });
    install<&esp_random>([] {
        if (state().random.empty()) return state().counter++;
        const auto value = state().random.front();
        state().random.pop_front();
        return value;
    });
    install<&esp_fill_random>([](void *buffer, size_t size) {
        auto *bytes = static_cast<uint8_t *>(buffer);
        for (size_t i = 0; i < size; ++i) bytes[i] = uint8_t(++state().fill | 1);
    });
    install<&config_store::read>([](const char *key, std::string &value) {
        const auto error = state().read_errors.find(key);
        if (error != state().read_errors.end()) return error->second;
        const auto found = state().saved.find(key);
        if (found == state().saved.end()) return ESP_ERR_NVS_NOT_FOUND;
        value = found->second;
        return ESP_OK;
    });
    install<&config_store::write>([](const char *key, const std::string &value) {
        state().writes.push_back(key);
        const auto error = state().write_errors.find(key);
        if (error != state().write_errors.end()) return error->second;
        state().saved[key] = value;
        return ESP_OK;
    });
    // The SRP verifier of a setup code; its bytes record the code.
    install<&amity_homekit_generate_setup>([](const char *code, hap_setup_info_t *setup) {
        CHECK(code && std::strlen(code) == 10);
        *setup = setup_info(uint8_t(code[0] + code[9]));
        std::memcpy(setup->verifier, code, 10);
        return ESP_OK;
    });
    install<&ble_remote_get_status>([](ble_remote_status_t *status) { *status = state().remote; });
    install<&amity::HubService::playback_state>([](const amity::HubService &) { return state().playback; });
}

// Starts HomeKit as main.cpp does. The worker task runs while start() waits
// for its startup: it restores the saved setup, runs one loop pass for each
// of passes and leaves. A pass without a queued command waits a second.
inline esp_err_t start(amity::SystemControl &hub, const amity_matter_callbacks_t &callbacks,
                       std::deque<std::function<void()>> passes = {}) {
    state().passes = std::move(passes);
    esp_timer_create_args_t args{};
    args.callback = [](void *) {
        for (const auto &task : amity_fake::tasks())
            if (task.name == "homekit" && task.state == amity_fake::Task::State::created) amity_fake::run_task(task.handle);
    };
    args.name = "scheduler";
    esp_timer_handle_t scheduler = nullptr;
    CHECK(esp_timer_create(&args, &scheduler) == ESP_OK);
    CHECK(esp_timer_start_once(scheduler, 1000) == ESP_OK);
    const esp_err_t result = amity_homekit::start(hub, callbacks);
    if (esp_timer_is_active(scheduler)) CHECK(esp_timer_stop(scheduler) == ESP_OK);
    CHECK(esp_timer_delete(scheduler) == ESP_OK);
    return result;
}

// The accessory's services, in the order homekit.cpp creates them.
inline hap::Service &television() { return hap::service("D8"); }
inline hap::Service &speaker() { return hap::service("113"); }
inline hap::Service &battery() { return hap::service("96"); }
inline hap::Service &input(unsigned slot) { return hap::service("D9", slot); }

inline hap_val_t integer(int value) {
    hap_val_t result{};
    result.i = value;
    return result;
}
inline hap_val_t flag(bool value) {
    hap_val_t result{};
    result.b = value;
    return result;
}
inline hap_val_t text(const char *value) {
    hap_val_t result{};
    result.s = const_cast<char *>(value);
    return result;
}

}  // namespace homekit_harness
