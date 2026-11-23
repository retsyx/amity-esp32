/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Strict seams for the HomeKit SDK fork functions amity_hap.c calls, from
// esp_hap_ip_services.c, esp_hap_pair_verify.c, esp_hap_keystore.c and
// hap_platform_httpd.c, and the SDK's database, hap_priv, which
// esp_hap_database.c defines.
extern "C" {
#include "esp_hap_database.h"
#include "esp_hap_ip_services.h"
#include "esp_hap_keystore.h"
#include "esp_hap_pair_verify.h"
#include "hap_platform_httpd.h"
}
#include "seams.hpp"

hap_priv_t hap_priv;

int hap_mdns_announce(bool first) { return AMITY_SEAM(hap_mdns_announce, first); }
int hap_mdns_deannounce() { return AMITY_SEAM(hap_mdns_deannounce); }
int hap_mdns_deannounce_for_rename(void) { return AMITY_SEAM(hap_mdns_deannounce_for_rename); }
int hap_ip_services_stop() { return AMITY_SEAM(hap_ip_services_stop); }
void hap_close_all_sessions() { AMITY_SEAM(hap_close_all_sessions); }
int hap_keystore_delete_namespace(const char *name_space) { return AMITY_SEAM(hap_keystore_delete_namespace, name_space); }
int hap_platform_httpd_stop(httpd_handle_t *handle) { return AMITY_SEAM(hap_platform_httpd_stop, handle); }
