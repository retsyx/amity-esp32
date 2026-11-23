/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The station interface's addresses and hostname, and the IP events, with the
// pinned ESP-IDF's types and constants (checked by sdk_parity/main.def). The
// functions are strict seams.
#include "esp_err.h"
#include "esp_event.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct esp_netif_obj;
typedef struct esp_netif_obj esp_netif_t;

struct esp_ip6_addr {
    uint32_t addr[4];
    uint8_t zone;
};
struct esp_ip4_addr {
    uint32_t addr;
};
typedef struct esp_ip4_addr esp_ip4_addr_t;
typedef struct esp_ip6_addr esp_ip6_addr_t;
typedef struct _ip_addr {
    union {
        esp_ip6_addr_t ip6;
        esp_ip4_addr_t ip4;
    } u_addr;
    uint8_t type;
} esp_ip_addr_t;

#define ESP_IPADDR_TYPE_V4 0U
#define ESP_IPADDR_TYPE_V6 6U

#define IPSTR "%d.%d.%d.%d"
#define esp_ip4_addr_get_byte(ipaddr, idx) (((const uint8_t *)(&(ipaddr)->addr))[idx])
#define esp_ip4_addr1_16(ipaddr) ((uint16_t)esp_ip4_addr_get_byte(ipaddr, 0))
#define esp_ip4_addr2_16(ipaddr) ((uint16_t)esp_ip4_addr_get_byte(ipaddr, 1))
#define esp_ip4_addr3_16(ipaddr) ((uint16_t)esp_ip4_addr_get_byte(ipaddr, 2))
#define esp_ip4_addr4_16(ipaddr) ((uint16_t)esp_ip4_addr_get_byte(ipaddr, 3))
#define IP2STR(ipaddr) esp_ip4_addr1_16(ipaddr), esp_ip4_addr2_16(ipaddr), esp_ip4_addr3_16(ipaddr), esp_ip4_addr4_16(ipaddr)

typedef enum {
    IP_EVENT_STA_GOT_IP,
    IP_EVENT_STA_LOST_IP,
    IP_EVENT_ASSIGNED_IP_TO_CLIENT,
    IP_EVENT_GOT_IP6,
} ip_event_t;
ESP_EVENT_DECLARE_BASE(IP_EVENT);

typedef struct {
    esp_ip4_addr_t ip;
    esp_ip4_addr_t netmask;
    esp_ip4_addr_t gw;
} esp_netif_ip_info_t;

typedef struct {
    esp_netif_t *esp_netif;
    esp_netif_ip_info_t ip_info;
    bool ip_changed;
} ip_event_got_ip_t;

esp_netif_t *esp_netif_get_handle_from_ifkey(const char *if_key);
bool esp_netif_is_netif_up(esp_netif_t *esp_netif);
esp_err_t esp_netif_get_ip_info(esp_netif_t *esp_netif, esp_netif_ip_info_t *ip_info);
int esp_netif_get_all_preferred_ip6(esp_netif_t *esp_netif, esp_ip6_addr_t if_ip6[]);
esp_err_t esp_netif_set_hostname(esp_netif_t *esp_netif, const char *hostname);

#ifdef __cplusplus
}
#endif
