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
static SemaphoreHandle_t lock;
static char hostname[33];
static int64_t pairing_deadline;
static uint32_t pairing_window;
int hap_amity_init(const char *name) {
    if (!name || strlen(name) >= sizeof(hostname)) return HAP_FAIL;
    if (!lock) lock = xSemaphoreCreateRecursiveMutex();
    if (!lock) return HAP_FAIL;
    strcpy(hostname, name);
    return HAP_SUCCESS;
}
void hap_amity_lock(void) { xSemaphoreTakeRecursive(lock, portMAX_DELAY); }
void hap_amity_unlock(void) { xSemaphoreGiveRecursive(lock); }
const char *hap_amity_hostname(void) { return hostname; }
void hap_amity_set_pairing_deadline(int64_t deadline) {
    if (deadline != pairing_deadline) ++pairing_window;
    pairing_deadline = deadline;
}
bool hap_amity_pairing_allowed(void) { return pairing_deadline > esp_timer_get_time(); }
uint32_t hap_amity_pairing_window(void) { return pairing_window; }
int hap_amity_refresh_mdns(void) { return hap_mdns_announce(false); }
int hap_amity_remove_pairings(void) {
    hap_close_all_sessions();
    if (hap_keystore_delete_namespace("hap_ctrl") != HAP_SUCCESS) return HAP_FAIL;
    memset(hap_priv.controllers, 0, sizeof(hap_priv.controllers));
    pairing_deadline = 0;
    return hap_mdns_deannounce();
}
