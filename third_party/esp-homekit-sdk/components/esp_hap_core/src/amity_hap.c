/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "amity_hap.h"
#include "esp_hap_database.h"
#include "esp_hap_controllers.h"
#include "esp_hap_ip_services.h"
#include "esp_hap_pair_verify.h"
#include "esp_hap_keystore.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdatomic.h>
#include "esp_log.h"
#include "hap_platform_httpd.h"
static SemaphoreHandle_t lock;
static char hostname[33];
static int64_t pairing_deadline;
static uint32_t pairing_window;
static atomic_bool service_enabled = true;
int hap_amity_init(const char *name) {
    if (!atomic_load(&service_enabled)) return HAP_FAIL;
    if (!name || strlen(name) >= sizeof(hostname)) return HAP_FAIL;
    if (!lock) lock = xSemaphoreCreateRecursiveMutex();
    if (!lock) return HAP_FAIL;
    strcpy(hostname, name);
    return HAP_SUCCESS;
}
void hap_amity_lock(void) { if (lock) xSemaphoreTakeRecursive(lock, portMAX_DELAY); }
void hap_amity_unlock(void) { if (lock) xSemaphoreGiveRecursive(lock); }
const char *hap_amity_hostname(void) { return hostname; }
void hap_amity_set_pairing_deadline(int64_t deadline) {
    if (deadline != pairing_deadline) ++pairing_window;
    pairing_deadline = deadline;
}
bool hap_amity_enabled(void) { return atomic_load(&service_enabled); }
bool hap_amity_pairing_allowed(void) { return hap_amity_enabled() && pairing_deadline > esp_timer_get_time(); }
uint32_t hap_amity_pairing_window(void) { return pairing_window; }
int hap_amity_refresh_mdns(void) { return hap_mdns_announce(false); }
int hap_amity_rename_hostname(const char *name) {
    if (!name || !name[0] || strlen(name) >= sizeof(hostname)) return HAP_FAIL;
    hap_amity_lock();
    if (!strcmp(hostname, name)) { hap_amity_unlock(); return HAP_SUCCESS; }
    char previous[sizeof(hostname)];
    strcpy(previous, hostname);
    int result = hap_mdns_deannounce_for_rename();
    if (result == HAP_SUCCESS) {
        strcpy(hostname, name);
        result = hap_mdns_announce(false);
        if (result != HAP_SUCCESS) {
            strcpy(hostname, previous);
            if (hap_mdns_announce(false) != HAP_SUCCESS)
                ESP_LOGE("amity_hap", "Could not restore HomeKit discovery after hostname change");
        }
    }
    hap_amity_unlock();
    return result;
}
int hap_amity_remove_pairings(void) {
    hap_close_all_sessions();
    if (hap_keystore_delete_namespace("hap_ctrl") != HAP_SUCCESS) return HAP_FAIL;
    memset(hap_priv.controllers, 0, sizeof(hap_priv.controllers));
    pairing_deadline = 0;
    return hap_mdns_deannounce();
}

int hap_amity_stop(void) {
    atomic_store(&service_enabled, false);
    if (!lock) return HAP_SUCCESS;
    hap_amity_lock();
    hap_amity_set_pairing_deadline(0);
    hap_close_all_sessions();
    int result = hap_mdns_deannounce();
    if (hap_ip_services_stop() != HAP_SUCCESS) result = HAP_FAIL;
    hap_amity_unlock();
    // httpd_stop waits for handler teardown. Never hold the HAP mutex here:
    // an existing handler can be waiting for it while the server exits.
    if (hap_priv.server) {
        if (hap_platform_httpd_stop(&hap_priv.server) != ESP_OK) return HAP_FAIL;
        hap_priv.server = NULL;
    }
    // Keep the SDK event loop inert rather than its asynchronous hap_stop
    // loop/queue destruction. No pairing/fabric namespace is erased.
    return result;
}
