/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The station query homekit.cpp uses to tell whether the network is up, with
// the SDK's access point record layout for the ESP32-S3 (no 5 GHz channels).
// The enumerations list only the values the fakes need.
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_ERR_WIFI_NOT_INIT (ESP_ERR_WIFI_BASE + 1)
#define ESP_ERR_WIFI_CONN (ESP_ERR_WIFI_BASE + 7)
#define ESP_ERR_WIFI_NOT_CONNECT (ESP_ERR_WIFI_BASE + 15)

typedef enum { WIFI_SECOND_CHAN_NONE = 0 } wifi_second_chan_t;
typedef enum { WIFI_AUTH_OPEN = 0 } wifi_auth_mode_t;
typedef enum { WIFI_CIPHER_TYPE_NONE = 0 } wifi_cipher_type_t;
typedef enum { WIFI_ANT_ANT0 = 0 } wifi_ant_t;
typedef enum { WIFI_COUNTRY_POLICY_AUTO = 0 } wifi_country_policy_t;
typedef enum { WIFI_BW20 = 1 } wifi_bandwidth_t;

typedef struct {
    char cc[3];
    uint8_t schan;
    uint8_t nchan;
    int8_t max_tx_power;
    wifi_country_policy_t policy;
} wifi_country_t;

typedef struct {
    uint8_t bss_color : 6;
    uint8_t partial_bss_color : 1;
    uint8_t bss_color_disabled : 1;
    uint8_t bssid_index;
} wifi_he_ap_info_t;

typedef struct {
    uint8_t bssid[6];
    uint8_t ssid[33];
    uint8_t primary;
    wifi_second_chan_t second;
    int8_t rssi;
    wifi_auth_mode_t authmode;
    wifi_cipher_type_t pairwise_cipher;
    wifi_cipher_type_t group_cipher;
    wifi_ant_t ant;
    uint32_t phy_11b : 1;
    uint32_t phy_11g : 1;
    uint32_t phy_11n : 1;
    uint32_t phy_lr : 1;
    uint32_t phy_11a : 1;
    uint32_t phy_11ac : 1;
    uint32_t phy_11ax : 1;
    uint32_t wps : 1;
    uint32_t ftm_responder : 1;
    uint32_t ftm_initiator : 1;
    uint32_t akm_dpp : 1;
    uint32_t reserved : 21;
    wifi_country_t country;
    wifi_he_ap_info_t he_ap;
    wifi_bandwidth_t bandwidth;
    uint8_t vht_ch_freq1;
    uint8_t vht_ch_freq2;
} wifi_ap_record_t;

esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap_info);

#ifdef __cplusplus
}
#endif
