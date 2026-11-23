/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The station interface queries, strict seams.
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct esp_netif_obj;
typedef struct esp_netif_obj esp_netif_t;

struct esp_ip4_addr {
    uint32_t addr;
};
typedef struct esp_ip4_addr esp_ip4_addr_t;

typedef struct {
    esp_ip4_addr_t ip;
    esp_ip4_addr_t netmask;
    esp_ip4_addr_t gw;
} esp_netif_ip_info_t;

esp_netif_t *esp_netif_get_handle_from_ifkey(const char *if_key);
bool esp_netif_is_netif_up(esp_netif_t *esp_netif);
esp_err_t esp_netif_get_ip_info(esp_netif_t *esp_netif, esp_netif_ip_info_t *ip_info);

#ifdef __cplusplus
}
#endif
