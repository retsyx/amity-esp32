/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "controller.hpp"
#include "apple_fabrics.h"
#include "esp_log.h"

namespace amity_matter {

amity_matter_callbacks_t callbacks{};
amity_matter_state_t state{};
amity_matter_input_t listed_inputs[AMITY_MATTER_MAX_INPUTS]{};
std::atomic<bool> started{false}, stack_started{false}, controller_enabled{true};
std::mutex lifecycle_mutex;

namespace {
constexpr const char *tag = "amity_matter";

std::mutex state_publication_mutex;
amity_matter_state_t latest_state{};
bool state_dirty=false,state_publication_pending=false;
std::atomic<bool> controller_revoked{false};
std::atomic<bool> controller_enrollment_complete{false}, enrollment_check_pending{false};

void publish_state(intptr_t)
{
    // ScheduleWork runs on the CHIP task with its stack lock already held.
    // Copy the latest observation only now, after any unrelated CHIP work.
    amity_matter_state_t next;
    {
        std::lock_guard<std::mutex> guard(state_publication_mutex);
        state_publication_pending=false;
        if (!controller_enabled.load() || !started.load()) { state_dirty=false;return; }
        if (!state_dirty) return;
        next=latest_state;state_dirty=false;
    }
    const auto old=state;
    state=next;
    if (old.power != state.power)
        report(Attribute::power);
    if (old.current_input != state.current_input)
        report(Attribute::current_input);
    if (old.config_revision != state.config_revision || old.presentation_revision != state.presentation_revision)
        report(Attribute::input_list);
    if (old.mute_known != state.mute_known || old.muted != state.muted)
        report(Attribute::mute);
    if (old.volume_known != state.volume_known || old.volume != state.volume)
        report(Attribute::volume);
}

void finish_controller_enrollment(intptr_t)
{
    enrollment_check_pending.store(false);
    if (!controller_enabled.load() || !started.load()) return;
    if (!chip_port::fail_safe_disarmed() ||
        !chip_port::has_fabric() ||
        !chip_port::station_provisioned() ||
        !chip_port::station_connected()) return;
    // NetworkCommissioningCluster handles the same completion event, but its
    // commit error is ignored by the SDK. Check it explicitly before role CAS.
    if (!chip_port::commit_wifi()) {
        ESP_LOGE(tag, "Controller enrollment Wi-Fi commit failed; user interfaces remain closed");
        return;
    }
    controller_enrollment_complete.store(true);
}
}

void report(Attribute attribute)
{
    if (controller_enabled.load() && started.load()) chip_port::report(attribute);
}

esp_err_t dispatch_key(uint8_t key)
{
    return controller_enabled.load() && callbacks.key ? callbacks.key(key, callbacks.context) : ESP_ERR_INVALID_STATE;
}

size_t current_inputs()
{
    const size_t count = callbacks.inputs ? callbacks.inputs(listed_inputs, AMITY_MATTER_MAX_INPUTS, callbacks.context) : 0;
    return count <= AMITY_MATTER_MAX_INPUTS ? count : 0;
}

}

using namespace amity_matter;

extern "C" bool amity_matter_controller_enrollment_complete(void)
{
    if (!controller_enabled.load() || !started.load()) return false;
    if (controller_enrollment_complete.load()) return true;
    // Polling also recovers a retained legacy fabric after Wi-Fi reconnects.
    // Keep at most one verification callback queued on the CHIP task.
    bool expected = false;
    if (enrollment_check_pending.compare_exchange_strong(expected, true) &&
        !chip_port::schedule(finish_controller_enrollment))
        enrollment_check_pending.store(false);
    return false;
}

extern "C" esp_err_t amity_matter_update_state(const amity_matter_state_t *next)
{
    if (!next || (next->volume_known && next->volume > 254)) return ESP_ERR_INVALID_ARG;
    {
        std::lock_guard<std::mutex> guard(state_publication_mutex);
        if (!controller_enabled.load()) return ESP_OK;
        latest_state=*next;
        if (!started.load()) { state=*next;return ESP_OK; }
        state_dirty=true;
    }
    return amity_matter_flush_state();
}

extern "C" esp_err_t amity_matter_flush_state(void)
{
    {
        std::lock_guard<std::mutex> guard(state_publication_mutex);
        if (!started.load() || !controller_enabled.load() || !state_dirty || state_publication_pending)
            return ESP_OK;
        state_publication_pending=true;
    }
    // The pinned platform implementation waits at most one RTOS tick for
    // queue admission; it never acquires the CHIP stack lock here.
    if (chip_port::schedule(publish_state)) return ESP_OK;
    std::lock_guard<std::mutex> guard(state_publication_mutex);
    state_publication_pending=false; // Keep the dirty final observation for the background retry.
    return ESP_FAIL;
}

extern "C" esp_err_t amity_matter_set_controller_enabled(bool enabled) {
    if (!enabled) { controller_revoked.store(true); controller_enabled.store(false); } // revoke before waiting
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex);
    if (enabled) {
        if (controller_revoked.load()) return ESP_ERR_INVALID_STATE;
        controller_enabled.store(true); return ESP_OK;
    }
    if (!stack_started.load() || !started.load()) return ESP_OK;
    chip_port::with_stack_lock([] {
        chip_port::stop_dns_sd();
        chip_port::unregister_attribute_access();
        amity_matter::stop_apple_fabric_cleanup();
        chip_port::shutdown_server();
        started.store(false);
        ESP_LOGI(tag, "Matter user services stopped; fabrics and shared network/BLE host retained");
    });
    return ESP_OK;
}

// The checked-in link wrapper also fences ESP-Matter's private IP-event callback.
// The pinned SDK would otherwise restart home discovery after Server shutdown.
extern "C" void __real__ZN4chip3app11DnssdServer11StartServerEv(void*);
extern "C" void __wrap__ZN4chip3app11DnssdServer11StartServerEv(void* self) {
    if (controller_enabled.load()) __real__ZN4chip3app11DnssdServer11StartServerEv(self);
}
