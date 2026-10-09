/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "diagnostics.h"
#include "diagnostics_support.hpp"
#include "system_services.hpp"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <platform/ESP32/DiagnosticDataProviderImpl.h>
#include <platform/DiagnosticDataProvider.h>
#include <app/clusters/diagnostic-logs-server/DiagnosticLogsProviderDelegate.h>

namespace {
using namespace chip;
using namespace chip::app::Clusters;
nvs_handle_t hours_storage = 0;
uint64_t saved_seconds = 0;
SemaphoreHandle_t hours_lock = nullptr;

void checkpoint_hours() {
    if (!hours_storage || !hours_lock) return;
    // NVS survives OTA; hourly checkpoints bound loss on unexpected power failure.
    xSemaphoreTake(hours_lock, portMAX_DELAY);
    const auto now = amity_diagnostics::operational_seconds(saved_seconds, esp_timer_get_time() / 1000000);
    auto err = nvs_set_u64(hours_storage, "seconds", now);
    if (err == ESP_OK) err = nvs_commit(hours_storage);
    xSemaphoreGive(hours_lock);
    if (err != ESP_OK) ESP_LOGW("amity_diagnostics", "Operating time checkpoint failed: %s", esp_err_to_name(err));
}
void hours_timer(void *) { checkpoint_hours(); }

class Diagnostics final : public DeviceLayer::DiagnosticDataProviderImpl {
public:
    CHIP_ERROR GetTotalOperationalHours(uint32_t &value) override {
        value = amity_diagnostics::operational_hours(saved_seconds, esp_timer_get_time() / 1000000);
        return CHIP_NO_ERROR;
    }
} diagnostic_provider;

class Logs final : public DiagnosticLogs::DiagnosticLogsProviderDelegate {
    amity_diagnostics::LogSessions sessions;
    static bool available(DiagnosticLogs::IntentEnum intent) {
        return intent == DiagnosticLogs::IntentEnum::kEndUserSupport || intent == DiagnosticLogs::IntentEnum::kNetworkDiag;
    }
public:
    CHIP_ERROR StartLogCollection(DiagnosticLogs::IntentEnum intent, DiagnosticLogs::LogSessionHandle &handle,
                                  Optional<uint64_t> &timestamp, Optional<uint64_t> &since_boot) override {
        handle = DiagnosticLogs::kInvalidLogSessionHandle;
        timestamp.ClearValue(); since_boot.ClearValue();
        if (!available(intent)) return CHIP_ERROR_NOT_FOUND;
        auto snapshot = system_services::logs();
        if (snapshot.empty()) return CHIP_ERROR_NOT_FOUND;
        handle = sessions.begin(std::move(snapshot));
        return handle == DiagnosticLogs::kInvalidLogSessionHandle ? CHIP_ERROR_NO_MEMORY : CHIP_NO_ERROR;
    }
    CHIP_ERROR EndLogCollection(DiagnosticLogs::LogSessionHandle handle, CHIP_ERROR) override {
        return sessions.end(handle) ? CHIP_NO_ERROR : CHIP_ERROR_INVALID_ARGUMENT;
    }
    CHIP_ERROR CollectLog(DiagnosticLogs::LogSessionHandle handle, MutableByteSpan &buffer, bool &finished) override {
        size_t length = buffer.size();
        if (!sessions.collect(handle, buffer.data(), length, finished)) return CHIP_ERROR_INVALID_ARGUMENT;
        buffer.reduce_size(length);
        return CHIP_NO_ERROR;
    }
    size_t GetSizeForIntent(DiagnosticLogs::IntentEnum intent) override {
        return available(intent) ? system_services::log_size() : 0;
    }
    CHIP_ERROR GetLogForIntent(DiagnosticLogs::IntentEnum intent, MutableByteSpan &buffer,
                               Optional<uint64_t> &timestamp, Optional<uint64_t> &since_boot) override {
        timestamp.ClearValue(); since_boot.ClearValue();
        if (!available(intent)) return CHIP_ERROR_NOT_FOUND;
        const auto snapshot = system_services::logs();
        if (snapshot.empty()) return CHIP_ERROR_NOT_FOUND;
        // ResponsePayload carries the newest retained bytes. BDX streams the
        // entire stable snapshot through StartLogCollection/CollectLog instead.
        const size_t size = amity_diagnostics::copy_recent(snapshot, buffer.data(), buffer.size());
        buffer.reduce_size(size);
        return CHIP_NO_ERROR;
    }
} log_provider;

}

namespace amity_diagnostics {
esp_err_t configure(esp_matter::node_t *node) {
    using namespace esp_matter;
    auto *root = endpoint::get(node, 0);
    auto *general = cluster::get(root, GeneralDiagnostics::Id);
    auto *wifi = cluster::get(root, WiFiNetworkDiagnostics::Id);
    if (!general || !wifi) return ESP_ERR_INVALID_STATE;
    if (!cluster::general_diagnostics::attribute::create_total_operational_hours(general, 0) ||
        !cluster::general_diagnostics::attribute::create_boot_reason(general, 0) ||
        !cluster::general_diagnostics::event::create_hardware_fault_change(general) ||
        !cluster::general_diagnostics::event::create_radio_fault_change(general) ||
        !cluster::general_diagnostics::event::create_network_fault_change(general)) return ESP_ERR_NO_MEM;
    cluster::diagnostic_logs::config_t logs_config;
    logs_config.delegate = &log_provider;
    if (!cluster::diagnostic_logs::create(root, &logs_config, CLUSTER_FLAG_SERVER)) return ESP_ERR_NO_MEM;
    if (!cluster::wifi_network_diagnostics::event::create_disconnection(wifi) ||
        !cluster::wifi_network_diagnostics::event::create_association_failure(wifi) ||
        !cluster::wifi_network_diagnostics::event::create_connection_status(wifi)) return ESP_ERR_NO_MEM;

    auto err = nvs_open("amity_diag", NVS_READWRITE, &hours_storage);
    if (err != ESP_OK) return err;
    err = nvs_get_u64(hours_storage, "seconds", &saved_seconds);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    hours_lock = xSemaphoreCreateMutex();
    if (!hours_lock) return ESP_ERR_NO_MEM;
    err = esp_register_shutdown_handler(checkpoint_hours);
    if (err != ESP_OK) return err;
    esp_timer_create_args_t timer_config{};
    timer_config.callback = hours_timer;
    timer_config.name = "diagnostic_hours";
    timer_config.skip_unhandled_events = true;
    esp_timer_handle_t timer;
    err = esp_timer_create(&timer_config, &timer);
    if (err != ESP_OK) return err;
    err = esp_timer_start_periodic(timer, 3600ULL * 1000000);
    if (err != ESP_OK) return err;
    DeviceLayer::SetDiagnosticDataProvider(&diagnostic_provider);
    return ESP_OK;
}
}
