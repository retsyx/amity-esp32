/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Strict seams for what homekit.cpp calls outside the files the HomeKit tests
// build: config_store's NVS entries, the configuration save, the Amity side
// of the HomeKit SDK fork (amity_hap.c) and the SDK's Wi-Fi, MAC and random
// number functions.
#include "amity/system_sync.hpp"
#include "amity_hap.h"
#include "config_store.hpp"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "seams.hpp"

esp_err_t config_store::read(const char *key, std::string &value) { return AMITY_SEAM(config_store::read, key, value); }
esp_err_t config_store::write(const char *key, const std::string &value) {
    return AMITY_SEAM(config_store::write, key, value);
}

esp_err_t amity::activate_distributed_configuration(config_store::Configuration candidate, config_store::RevisionPair expected,
                                                    system::MessageId request, std::function<esp_err_t()> before_commit) {
    return AMITY_SEAM(amity::activate_distributed_configuration, std::move(candidate), expected, request,
                      std::move(before_commit));
}

int hap_amity_init(const char *hostname) { return AMITY_SEAM(hap_amity_init, hostname); }
void hap_amity_lock(void) { AMITY_SEAM(hap_amity_lock); }
void hap_amity_unlock(void) { AMITY_SEAM(hap_amity_unlock); }
const char *hap_amity_hostname(void) { return AMITY_SEAM(hap_amity_hostname); }
void hap_amity_set_pairing_deadline(int64_t deadline) { AMITY_SEAM(hap_amity_set_pairing_deadline, deadline); }
bool hap_amity_pairing_allowed(void) { return AMITY_SEAM(hap_amity_pairing_allowed); }
uint32_t hap_amity_pairing_window(void) { return AMITY_SEAM(hap_amity_pairing_window); }
int hap_amity_refresh_mdns(void) { return AMITY_SEAM(hap_amity_refresh_mdns); }
int hap_amity_rename_hostname(const char *hostname) { return AMITY_SEAM(hap_amity_rename_hostname, hostname); }
int hap_amity_remove_pairings(void) { return AMITY_SEAM(hap_amity_remove_pairings); }
bool hap_amity_enabled(void) { return AMITY_SEAM(hap_amity_enabled); }
int hap_amity_stop(void) { return AMITY_SEAM(hap_amity_stop); }

esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap_info) { return AMITY_SEAM(esp_wifi_sta_get_ap_info, ap_info); }
esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type) { return AMITY_SEAM(esp_read_mac, mac, type); }
uint32_t esp_random(void) { return AMITY_SEAM(esp_random); }
void esp_fill_random(void *buf, size_t len) { AMITY_SEAM(esp_fill_random, buf, len); }
