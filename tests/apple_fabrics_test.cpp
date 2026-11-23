/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// The Apple Keychain fabric cleanup of components/amity_matter/apple_fabrics.cpp,
// built whole against the CHIP fakes. Each scenario builds its own AppleFabrics,
// except the one for the public start and stop functions.
#include "../components/amity_matter/apple_fabrics.cpp"
#include "amity_matter_fakes.hpp"
#include "core_fakes.hpp"
#include "esp_matter.h"
#include "freertos/FreeRTOS.h"
#include "scenario_main.hpp"
#include "seams.hpp"
#include <algorithm>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace {

using amity_matter::AppleFabrics;
using amity_matter::apple_home_vendor;
using amity_matter::apple_keychain_vendor;
using chip::FabricIndex;
using chip::FabricInfo;
using chip::VendorId;

VendorId vendor(uint16_t id) { return static_cast<VendorId>(id); }

// The NVS namespace holding the pending marker. After the server shuts down,
// nothing may write it.
struct Storage {
    bool present = true;
    uint8_t pending = 1;
    esp_err_t open_error = ESP_OK, read_error = ESP_OK;
    unsigned writes = 0, commits = 0;
} storage;
bool server_down = false;

// The fabric table as the SDK keeps it: Delete tells the delegate before and
// after removing a fabric. Every call needs the stack lock, and nothing may
// delete a fabric after the server shuts down.
struct Table {
    std::vector<FabricInfo> fabrics{{1, vendor(apple_keychain_vendor)}};
    FabricIndex pending_index = chip::kUndefinedFabricIndex;
    chip::FabricTable::Delegate *delegate = nullptr;
    CHIP_ERROR add_result = CHIP_NO_ERROR;
    std::deque<CHIP_ERROR> delete_results;
    std::vector<FabricIndex> deleted;
} table;

void script_table() {
    using chip::FabricTable;
    amity_seam::install<&FabricTable::begin>([](const FabricTable &) {
        CHECK(amity_fake::chip_stack_locked());
        return table.fabrics.data();
    });
    amity_seam::install<&FabricTable::end>([](const FabricTable &) { return table.fabrics.data() + table.fabrics.size(); });
    amity_seam::install<&FabricTable::GetPendingNewFabricIndex>([](const FabricTable &) { return table.pending_index; });
    amity_seam::install<&FabricTable::FindFabricWithIndex>([](const FabricTable &, FabricIndex index) -> const FabricInfo * {
        CHECK(amity_fake::chip_stack_locked());
        for (const auto &fabric : table.fabrics)
            if (fabric.GetFabricIndex() == index) return &fabric;
        return nullptr;
    });
    amity_seam::install<&FabricTable::AddFabricDelegate>([](FabricTable &, FabricTable::Delegate *delegate) {
        CHECK(amity_fake::chip_stack_locked() && !table.delegate);
        if (table.add_result == CHIP_NO_ERROR) table.delegate = delegate;
        return table.add_result;
    });
    amity_seam::install<&FabricTable::RemoveFabricDelegate>([](FabricTable &, FabricTable::Delegate *delegate) {
        CHECK(amity_fake::chip_stack_locked() && table.delegate == delegate);
        table.delegate = nullptr;
    });
    amity_seam::install<&FabricTable::Delete>([](FabricTable &self, FabricIndex index) {
        CHECK(amity_fake::chip_stack_locked() && !server_down);
        if (!table.delete_results.empty()) {
            const auto result = table.delete_results.front();
            table.delete_results.pop_front();
            if (result != CHIP_NO_ERROR) return result;
        }
        if (table.delegate) table.delegate->FabricWillBeRemoved(self, index);
        table.fabrics.erase(std::remove_if(table.fabrics.begin(), table.fabrics.end(),
                                           [&](const FabricInfo &fabric) { return fabric.GetFabricIndex() == index; }),
                            table.fabrics.end());
        table.deleted.push_back(index);
        if (table.delegate) table.delegate->OnFabricRemoved(self, index);
        return CHIP_NO_ERROR;
    });
}

bool fail_safe_armed = false;

void script_server() {
    script_table();
    amity_seam::install<&chip::app::FailSafeContext::IsFailSafeFullyDisarmed>(
        [](const chip::app::FailSafeContext &) { return !fail_safe_armed; });
}

esp_err_t locked_start(AppleFabrics &worker) {
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    return worker.start();
}

void locked_stop(AppleFabrics &worker) {
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    worker.stop();
}

// Shuts the server down after stop(): later CHIP work must not touch the table or NVS.
void shut_down() { server_down = true; }

bool warned(const std::string &text) {
    for (const auto &line : amity_fake::log_lines())
        if (line.level == ESP_LOG_WARN && line.tag == "apple_fabrics" && line.message == text) return true;
    return false;
}

// stop() removes the delegate and fences CHIP work queued before it, so after
// the server shuts down nothing deletes a fabric or rewrites the pending
// marker. A second stop() does nothing.
void stop_fences_queued_work() {
    script_server();
    AppleFabrics worker;
    CHECK(locked_start(worker) == ESP_OK);
    CHECK(table.delegate == &worker && amity_fake::queued_chip_work() == 1);
    locked_stop(worker);
    CHECK(!table.delegate && amity_fake::chip_timer_cancels() == 1);
    shut_down();
    CHECK(amity_fake::run_all_chip_work() == 1);
    CHECK(table.deleted.empty() && storage.writes == 0 && storage.pending == 1);
    locked_stop(worker);
    CHECK(amity_seam::count("chip::FabricTable::RemoveFabricDelegate") == 1);
    CHECK(amity_fake::chip_timer_cancels() == 1);
}

// An armed fail-safe defers cleanup to a one-second retry timer, which stop()
// cancels: the timer of this callback and object.
void fail_safe_retry() {
    script_server();
    fail_safe_armed = true;
    AppleFabrics worker;
    CHECK(locked_start(worker) == ESP_OK);
    CHECK(amity_fake::run_chip_work());
    const auto timers = amity_fake::chip_timers();
    CHECK(timers.size() == 1 && timers[0].context == &worker);
    CHECK(timers[0].delay == chip::System::Clock::Seconds32(1));
    CHECK(table.deleted.empty() && storage.writes == 0);
    locked_stop(worker);
    CHECK(amity_fake::chip_timers().empty());
    shut_down();
    CHECK(amity_fake::run_all_chip_work() == 0 && !amity_fake::fire_chip_timer());
    CHECK(table.deleted.empty() && storage.writes == 0 && storage.pending == 1);
}

// With no Apple Home left, cleanup deletes the Apple Keychain fabric once and
// clears the persisted pending marker.
void cleanup() {
    script_server();
    table.fabrics.push_back({2, vendor(0x6006)});
    AppleFabrics worker;
    CHECK(locked_start(worker) == ESP_OK);
    CHECK(amity_fake::run_all_chip_work() == 1);
    CHECK(table.deleted == std::vector<FabricIndex>{1} && table.fabrics.size() == 1);
    CHECK(storage.pending == 0 && storage.writes == 1 && storage.commits == 1);
    CHECK(amity_fake::logged("Removed Apple Keychain fabric after last Apple Home removal"));
    CHECK(amity_fake::chip_timers().empty());
    locked_stop(worker);
}

// The public functions start and stop the one cleanup the firmware runs.
void public_functions() {
    script_server();
    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        CHECK(amity_matter::start_apple_fabric_cleanup() == ESP_OK);
        CHECK(table.delegate == &amity_matter::apple_fabrics && amity_fake::queued_chip_work() == 1);
        amity_matter::stop_apple_fabric_cleanup();
        CHECK(!table.delegate);
    }
    shut_down();
    CHECK(amity_fake::run_all_chip_work() == 1 && table.deleted.empty() && storage.writes == 0);
}

// A storage failure stops start(); a missing marker means nothing is pending.
void storage_errors() {
    script_server();
    {
        storage.open_error = ESP_ERR_NO_MEM;
        AppleFabrics worker;
        CHECK(locked_start(worker) == ESP_ERR_NO_MEM && !table.delegate);
    }
    {
        storage.open_error = ESP_OK;
        storage.read_error = ESP_ERR_NVS_NOT_INITIALIZED;
        AppleFabrics worker;
        CHECK(locked_start(worker) == ESP_ERR_NVS_NOT_INITIALIZED && !table.delegate);
    }
    storage.read_error = ESP_OK;
    storage.present = false;
    AppleFabrics worker;
    CHECK(locked_start(worker) == ESP_OK && table.delegate == &worker);
    CHECK(amity_fake::scheduled_chip_work() == 0 && amity_fake::queued_chip_work() == 0);
    locked_stop(worker);
}

// When the table refuses the delegate, start() fails before scheduling cleanup.
void delegate_refused() {
    script_server();
    table.add_result = CHIP_ERROR_NO_MEMORY;
    AppleFabrics worker;
    CHECK(locked_start(worker) == ESP_FAIL);
    CHECK(amity_fake::scheduled_chip_work() == 0 && amity_fake::chip_timers().empty());
    locked_stop(worker);
    CHECK(amity_seam::count("chip::FabricTable::RemoveFabricDelegate") == 0);
}

// When the CHIP queue refuses the work, a retry timer schedules it again.
void scheduling_retry() {
    script_server();
    amity_fake::fail_chip_scheduling(1);
    AppleFabrics worker;
    CHECK(locked_start(worker) == ESP_OK);
    CHECK(warned("Cannot schedule Apple cleanup: b"));
    CHECK(amity_fake::queued_chip_work() == 0 && amity_fake::chip_timers().size() == 1);
    CHECK(amity_fake::fire_chip_timer());
    CHECK(amity_fake::queued_chip_work() == 1 && amity_fake::run_all_chip_work() == 1);
    CHECK(table.deleted == std::vector<FabricIndex>{1} && storage.pending == 0);
    locked_stop(worker);
}

// A failed deletion keeps the marker and retries after a second.
void deletion_retry() {
    script_server();
    table.delete_results.push_back(CHIP_ERROR_INTERNAL);
    AppleFabrics worker;
    CHECK(locked_start(worker) == ESP_OK);
    CHECK(amity_fake::run_all_chip_work() == 1);
    CHECK(warned("Apple Keychain cleanup failed: ac"));
    CHECK(table.deleted.empty() && storage.writes == 0 && amity_fake::chip_timers().size() == 1);
    CHECK(amity_fake::fire_chip_timer() && amity_fake::run_all_chip_work() == 1);
    CHECK(table.deleted == std::vector<FabricIndex>{1} && storage.pending == 0 && storage.writes == 1);
    locked_stop(worker);
}

// Removing the last committed Apple Home persists the intent before the SDK
// deletes it, and the removal schedules cleanup of the Keychain fabric.
void last_home_removed() {
    script_server();
    storage.pending = 0;
    table.fabrics.push_back({2, vendor(apple_home_vendor)});
    AppleFabrics worker;
    CHECK(locked_start(worker) == ESP_OK && amity_fake::scheduled_chip_work() == 0);
    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        CHECK(chip::Server::GetInstance().GetFabricTable().Delete(2) == CHIP_NO_ERROR);
    }
    CHECK(storage.pending == 1 && storage.writes == 1 && storage.commits == 1);
    CHECK(amity_fake::queued_chip_work() == 1 && amity_fake::run_all_chip_work() == 1);
    CHECK(table.deleted == (std::vector<FabricIndex>{2, 1}) && table.fabrics.empty());
    CHECK(storage.pending == 0 && storage.writes == 2);
    locked_stop(worker);
}

// A Home committed before queued cleanup runs keeps the Keychain fabric; the
// marker is still cleared, and the work is queued once.
void home_committed() {
    script_server();
    AppleFabrics worker;
    CHECK(locked_start(worker) == ESP_OK && amity_fake::queued_chip_work() == 1);
    table.fabrics.push_back({3, vendor(apple_home_vendor)});
    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        worker.OnFabricCommitted(chip::Server::GetInstance().GetFabricTable(), 3);
    }
    CHECK(amity_fake::queued_chip_work() == 1 && amity_fake::run_all_chip_work() == 1);
    CHECK(table.deleted.empty() && storage.pending == 0 && storage.writes == 1);
    locked_stop(worker);
}

// A commit while cleanup waits out the fail-safe schedules it at once instead
// of after the retry delay.
void commit_reschedules() {
    script_server();
    fail_safe_armed = true;
    AppleFabrics worker;
    CHECK(locked_start(worker) == ESP_OK && amity_fake::run_all_chip_work() == 1);
    CHECK(amity_fake::chip_timers().size() == 1 && table.deleted.empty());
    fail_safe_armed = false;
    table.fabrics.push_back({5, vendor(0x6006)});
    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        worker.OnFabricCommitted(chip::Server::GetInstance().GetFabricTable(), 5);
    }
    CHECK(amity_fake::queued_chip_work() == 1 && amity_fake::run_all_chip_work() == 1);
    CHECK(table.deleted == std::vector<FabricIndex>{1} && storage.pending == 0);
    locked_stop(worker);
}

// A fabric still being commissioned is not a committed Home.
void pending_fabric_ignored() {
    script_server();
    table.fabrics.push_back({4, vendor(apple_home_vendor)});
    table.pending_index = 4;
    AppleFabrics worker;
    CHECK(locked_start(worker) == ESP_OK && amity_fake::run_all_chip_work() == 1);
    CHECK(table.deleted == std::vector<FabricIndex>{1} && storage.pending == 0);
    locked_stop(worker);
}

}  // namespace

// The NVS calls apple_fabrics.cpp makes, on its one namespace and key.
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle) {
    CHECK(std::strcmp(name, "amity_fabrics") == 0 && mode == NVS_READWRITE);
    if (storage.open_error != ESP_OK) return storage.open_error;
    *handle = 7;
    return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value) {
    CHECK(handle == 7 && std::strcmp(key, "apple_cleanup") == 0);
    if (storage.read_error != ESP_OK) return storage.read_error;
    if (!storage.present) return ESP_ERR_NVS_NOT_FOUND;
    *value = storage.pending;
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value) {
    CHECK(handle == 7 && std::strcmp(key, "apple_cleanup") == 0 && !server_down);
    storage.present = true;
    storage.pending = value;
    ++storage.writes;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) {
    CHECK(handle == 7 && !server_down);
    ++storage.commits;
    return ESP_OK;
}

int main(int argc, char **argv) {
    return amity_test::run(argc, argv,
                           {{"stop-fences-queued-work", stop_fences_queued_work},
                            {"fail-safe-retry", fail_safe_retry},
                            {"cleanup", cleanup},
                            {"public-functions", public_functions},
                            {"storage-errors", storage_errors},
                            {"delegate-refused", delegate_refused},
                            {"scheduling-retry", scheduling_retry},
                            {"deletion-retry", deletion_retry},
                            {"last-home-removed", last_home_removed},
                            {"home-committed", home_committed},
                            {"commit-reschedules", commit_reschedules},
                            {"pending-fabric-ignored", pending_fabric_ignored}});
}
